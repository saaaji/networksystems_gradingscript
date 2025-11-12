#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <ctype.h>
#include <time.h>
#include <openssl/evp.h>
#include <strings.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

// connection config
#define LISTEN_QUEUE_SIZE 1024
#define CLIENT_BUFFER_SIZE 8192
#define PROXY_CONNECTION_TIMEOUT_MS (10*1000)

// string constants
#define CR '\r'
#define LF '\n'

/**
 * MISC. UTILITIES
 */

static long TTL;

static char** blocklist = NULL;
static size_t blocklist_count = 0;
static pthread_mutex_t blocklist_lock = PTHREAD_MUTEX_INITIALIZER;

void load_blocklist(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;

    pthread_mutex_lock(&blocklist_lock);

    // free if existing
    if (blocklist) {
        for (size_t i = 0; i < blocklist_count; i++) {
            if (blocklist[i]) free(blocklist[i]);
        }
        free(blocklist);
    }

    blocklist = NULL;
    blocklist_count = 0;

    size_t capacity = 16;
    blocklist = malloc(capacity * sizeof(char*));
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (blocklist_count == capacity) {
            capacity *= 2;
            blocklist = realloc(blocklist, capacity * sizeof(char*));
        }

        blocklist[blocklist_count++] = strdup(line);
    }

    pthread_mutex_unlock(&blocklist_lock);
    fclose(f);
}

int is_blocked(const char* host) {
    pthread_mutex_lock(&blocklist_lock);
    for (size_t i = 0; i < blocklist_count; i++) {
        if (strcmp(host, blocklist[i]) == 0) {
            pthread_mutex_unlock(&blocklist_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&blocklist_lock);
    return 0;
}

typedef struct {
    char raw[CLIENT_BUFFER_SIZE];
    size_t len;
} Buffer;

typedef enum {
    kOk = 0,
    kErr = 1
} Result;

#define OK(value) ((value) == kOk)
#define OK_OS(value) ((value) == 0)

typedef struct {
    struct sockaddr_in addr;
    socklen_t len;
    int fd;

    // keep-alive mechanism
    int proxy_keep_alive;
    struct timespec last_activity;

    size_t id;
} Client;

Result wait_client(int lfd, Client* c) {
    static size_t id = 0;

    c->fd = accept(lfd, (struct sockaddr*) &c->addr,  &c->len);

    if (c->fd < 0) {
        return kErr;
    }

    c->id = id++;
    return kOk;
}

void usage() {
    fprintf(stderr, "I: usage: ./proxy <port>\n");
}

Result str_to_long(const char* str, long* result) {
    // reset errno
    errno = 0;

    char* end_ptr;
    long value = strtol(str, &end_ptr, 10);

    // catch error cases
    if (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN)) {
        return kErr;
    }

    if (errno != 0 && value == 0) {
        return kErr;
    }

    // no digits found
    if (end_ptr == str) {
        return kErr;
    }

    // invalid character found in string
    if (end_ptr && *end_ptr != '\0') {
        return kErr;
    }

    *result = value;
    return kOk;
}

/**
 * HTTP PARSING UTILITIES
 */

typedef struct {
    char* name;
    char* value;
} DynamicHttpHeader;

typedef enum {
    HTTP_REQUEST,
    HTTP_RESPONSE
} HttpMessageType;

typedef struct {
    // preamble
    char* method;
    char* uri;
    char* version;

    char* code_str;
    char* status_message;
    
    // store arbitrary number of headers
    DynamicHttpHeader* headers;
    size_t header_count;
    size_t header_capacity;

    // body
    char* body;
    size_t body_length;

    HttpMessageType type;
} DynamicHttpRequest;

void http_free_header(DynamicHttpHeader* header) {
    if (!header) return;
    if (header->name) free(header->name);
    if (header->value) free(header->value);
}

void http_free_request(DynamicHttpRequest* req) {
    if (!req) return;
    if (req->method) free(req->method);
    if (req->uri) free(req->uri);
    if (req->version) free(req->version);
    if (req->status_message) free(req->status_message);
    if (req->code_str) free(req->code_str);

    if (req->headers) {
        for (size_t i = 0; i < req->header_count; i++) {
            http_free_header(&req->headers[i]);
        }
        free(req->headers);
    }

    if (req->body) free(req->body);
}

ssize_t http_find_eoh(char* str, size_t len) {
    for (size_t i = 0; i < len - 3; i++) {
        // look for double CRLF
        if (str[i+0] == CR && str[i+1] == LF &&
            str[i+2] == CR && str[i+3] == LF) {
            return i;
        }
    }

    return -1;
}

ssize_t http_find_crlf(const char* str, size_t len) {
    for (size_t i = 0; i < len - 1; i++) {
        if (str[i+0] == CR && str[i+1] == LF) {
            return i;
        }
    }

    return -1;
}

char* alloc_strcpy(const char* start, size_t len) {
    char* dst = (char*) malloc(len + 1);
    if (!dst) return NULL;

    memcpy(dst, start, len);
    dst[len] = '\0';
    return dst;
}

void trim_whitespace(char** str_ptr) {
    // trim at beginning
    char* s = *str_ptr;
    while (*s && isspace(*s)) s++;
    if (s != *str_ptr) memmove(*str_ptr, s, strlen(s) + 1);

    // trim at end
    size_t len = strlen(*str_ptr);
    while (len > 0 && isspace((*str_ptr)[len - 1])) {
        (*str_ptr)[len-1] = '\0';
        len--;
    }
}

Result http_parse_headers(DynamicHttpRequest* req, char* str, size_t len) {
    if (!req || !str) goto cleanup;

    /**
     * parse first line (method, uri, version) OR (version, code, status message)
     */

    ssize_t eol = http_find_crlf(str, len);
    if (eol < 0) goto cleanup;

    const char* ptr = str;
    const char* end_ptr = ptr + eol;
    char** fields[3] = {NULL, NULL, NULL};
    
    if (req->type == HTTP_REQUEST) {
        fields[0] = &req->method;
        fields[1] = &req->uri;
        fields[2] = &req->version;
    } else if (req->type == HTTP_RESPONSE) {
        fields[0] = &req->version;
        fields[1] = &req->code_str;
        fields[2] = &req->status_message;
    }

    for (int i = 0; i < 3; i++) {
        const char* next_space = memchr(ptr, ' ', end_ptr - ptr);
        const char* field_end = NULL;

        if (i < 2) {
            if (!next_space) goto cleanup;
            field_end = next_space;
        } else {
            field_end = end_ptr;
        }

        *fields[i] = alloc_strcpy(ptr, field_end - ptr);
        if (!*fields[i]) goto cleanup;
        ptr = field_end + 1; // increment pointer
    }

    if (ptr + 2 > str + len) return kErr;
    ptr = end_ptr + 2; // advance past CRLF

    /**
     * parse headers
     */
    end_ptr = str + len;
    while (ptr < end_ptr) {
        ssize_t eol = http_find_crlf(ptr, end_ptr - ptr);
        if (eol < 0) {
            fprintf(stderr, "W: could not find terminating CRLF in headers\n");
            goto cleanup;
        }

        const char* header_end = ptr + eol;
        size_t line_len = header_end - ptr;

        // blank line indicates end of headers (double CRLF)
        if (line_len == 0) {
            break;
        }

        const char* colon = memchr(ptr, ':', line_len);
        if (!colon) {
            fprintf(stderr, "W: could not find ':' in header string\n");
            goto cleanup;
        }

        size_t name_len = colon - ptr;
        size_t value_len = line_len - (name_len + 1);

        // resize headers if necessary
        if (req->header_count == req->header_capacity) {
            size_t new_cap = req->header_capacity ? req->header_capacity * 2 : 8;
            DynamicHttpHeader* new_headers = realloc(req->headers, new_cap * sizeof(DynamicHttpHeader));
            if (!new_headers) goto cleanup;

            req->headers = new_headers;
            req->header_capacity = new_cap;
        }

        DynamicHttpHeader* h = &req->headers[req->header_count];
        h->name = alloc_strcpy(ptr, name_len);
        h->value = alloc_strcpy(colon + 1, value_len);

        trim_whitespace(&h->name);
        trim_whitespace(&h->value);
        req->header_count++;

        ptr = header_end + 2; // advance past CRLF
    }

    return kOk;

cleanup:
    fprintf(stderr, "W: encountered HTTP header parsing error\n");
    for (int i = 0; i < 3; i++) {
        if (*fields[i]) free(*fields[i]);
        *fields[i] = NULL;
    }

    http_free_request(req);

    return kErr;
}

long query_int_header_with_default(DynamicHttpRequest* req, const char* name, long default_value) {
    for (size_t i = 0; i < req->header_count; i++) {
        DynamicHttpHeader* h = &req->headers[i];
        
        if (OK_OS(strncmp(h->name, name, strlen(name)))) {
            long value;
            Result res = str_to_long(h->value, &value);
            if (OK(res)) {
                return value;
            } else break;
        }
    }

    return default_value;
}

const char* query_str_header(DynamicHttpRequest* req, const char* name) {
    for (size_t i = 0; i < req->header_count; i++) {
        DynamicHttpHeader* h = &req->headers[i];
        
        if (OK_OS(strncmp(h->name, name, strlen(name)))) {
            return h->value;
        }
    }

    return NULL;
}

/**
 * CLIENT HANDLING
 */

void send_error(int fd, int code, const char* version) {
    const char* name;
    switch (code) {
        case 400: name = "Bad Request"; break;
        case 403: name = "Forbidden"; break;
        case 404: name = "Not Found"; break;
        default: name = "Internal Server Error"; break;
    }

    char body[1024];
    int content_length = snprintf(body, sizeof(body),
        "<html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1></body></html>",
        code, name, code, name
    );

    dprintf(fd, 
        "%s %d %s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "%s", 
        version, 
        code, 
        name, 
        content_length, 
        body);
}

ssize_t read_headers(int fd, Buffer* buf, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t offset = buf->len;

    while (1) {
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return ret;

        ssize_t n = recv(fd, buf->raw + offset, CLIENT_BUFFER_SIZE - offset - 1, 0);
        if (n <= 0) return n;

        offset += n;
        buf->len = offset;
        buf->raw[offset] = '\0';

        if (http_find_eoh(buf->raw, offset) >= 0) break;
    }

    return buf->len;
}

ssize_t read_body(int fd, Buffer* buf, size_t content_length, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t offset = buf->len;

    while (offset < content_length) {
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return ret;

        ssize_t n = recv(fd, buf->raw + offset, content_length - offset - 1, 0);
        if (n <= 0) return n;

        offset += n;
        buf->len = offset;
    }

    return buf->len;
}

void get_uri_key(const char* uri, char* out_key) {
    EVP_MD_CTX* md_ctx;
    unsigned char* md5_digest;
    unsigned int md5_digest_len = EVP_MD_size(EVP_md5());
    assert(md5_digest_len == 16);

    md_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md_ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(md_ctx, uri, strlen(uri));

    md5_digest = (unsigned char*) OPENSSL_malloc(md5_digest_len);
    EVP_DigestFinal_ex(md_ctx, md5_digest, &md5_digest_len);
    EVP_MD_CTX_free(md_ctx);

    for (size_t i = 0; i < md5_digest_len; i++) {
        sprintf(out_key + 2*i, "%02x", md5_digest[i]);
    }

    out_key[md5_digest_len*2] = '\0';
    OPENSSL_free(md5_digest);
}

void try_create_cache() {
    struct stat st;
    if (stat("cache", &st) < 0) {
        if (mkdir("cache", 0755) < 0) {
            perror("mkdir");
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "'cache' exists but is not a directory\n");
    }
}

int is_cached(DynamicHttpRequest* req, char** cache_path) {
    char cache_key[16*2+1] = {0};
    get_uri_key(req->uri, cache_key);

    char fullpath[2048];
    memset(fullpath, 0, sizeof(fullpath));
    snprintf(fullpath, sizeof(fullpath), "cache/%s", cache_key);

    *cache_path = strdup(fullpath);

    fprintf(stderr, "cache key: %s -> %s\n", req->uri, *cache_path);

    struct stat st;
    if (stat(fullpath, &st) == 0) {
        // check age
        time_t now = time(NULL);
        int age = now - st.st_mtime; // elapsed since last modified

        fprintf(stderr, "\tentry exists, age = %d (TTL = %ld)\n", age, TTL);

        return age < TTL;
    }

    // not cached
    return 0;
}

#define MIN_LINK_CAP 32

typedef enum {
    SEEK_HREF = 0,
    CONSUME_LINK = 1
} HrefSearchState;

typedef struct {
    char** links;
    size_t link_count;
    size_t link_cap;

    size_t href_progress;
    char end_char;

    HrefSearchState state;
} HrefParser;

void init_href_parser(HrefParser* p) {
    p->state = SEEK_HREF;
    p->links = NULL;
    p->link_cap = 0;
    p->href_progress = 0;
}

void free_href_parser(HrefParser* p) {
    if (p->links) {
        for (size_t i = 0; i < p->link_count; i++) {
            free(p->links[i]);
        }
        free(p->links);
    }
}

typedef enum {
    PRE_NONE = 0,
    PRE_HTTP = 1,
    PRE_HTTPS = 2,
    PRE_REL = 3
} UriPrefix;

UriPrefix should_prefetch(char* link, size_t len) {
    if (strncmp(link, "https://", 8) == 0) return PRE_NONE; // skip HTTPS for now
    if (strncmp(link, "http://", 7) == 0) return PRE_HTTP;
    
    // pass relative through
    if (len >= 1 && (link[0] == '/' || link[0] == '.' || isalnum(link[0])))
        return PRE_REL;

    return PRE_NONE;
}

char* format_link(DynamicHttpRequest* root_req, char* first, size_t len, UriPrefix pre) {
    if (pre == PRE_HTTP) {
        char* link = (char*) malloc(len + 1);
        memcpy(link, first, len);
        link[len] = '\0';
        return link;
    } else if (pre == PRE_REL) {
        // char link_base[2048];
        // memset(link_base, (int) '\0', sizeof(link_base));
        
        if (strncmp(first, "/", 1)) {
            char* link = (char*) malloc(len + 1);
            memcpy(link, first, len);
            link[len] = '\0';
            return link;
        } else {
            // relative
            const char* p = strstr(root_req->uri, "://");
            if (p) p = strchr(p + 3, '/'); // look for next slash
            const char* base_path = p ? p : "/";
            char base_dir[1024];
            strncpy(base_dir, base_path, sizeof(base_dir));
            char* slash = strrchr(base_dir, '/');
            if (slash) *(slash + 1) = '\0';
            
            size_t res_len = strlen(base_dir) + len;
            char* link = (char*) malloc(res_len + 1);
            memset(link, (int) '\0', res_len);
            snprintf(link, res_len, "%s%.*s", base_dir, (int) len, first);

            return link;
        }

        // char* link = strdup(link_base);
        // return link;
    }

    return NULL;
}

/**
 * PREFETCH GLOBAL STATE
 */
typedef struct {
    char* host;
    char* uri;
} PrefetchJob;

void free_job(PrefetchJob* job) {
    free(job->host);
    free(job->uri);
}

#define MAX_PREFETCH 32
static PrefetchJob pre_stack[MAX_PREFETCH];
static size_t pre_count = 0;
static pthread_mutex_t pre_lock = PTHREAD_MUTEX_INITIALIZER;

// look for and complete links in chunk
void scrape_links(DynamicHttpRequest* root_req, HrefParser* p, char* str, size_t len) {
    static const char* href = "href";
    static const size_t href_len = 4;

    size_t max_search_range = len - href_len;

    for (size_t i = 0; i < max_search_range; i++) {
        int found_href = 1;
        if (p->state == SEEK_HREF) {
            size_t j;
            for (j = p->href_progress; j < href_len; j++) {
                if (str[i+j] != href[j]) {
                    found_href = 0;
                    break;
                }
            }

            if (0 < j && j < href_len && i == max_search_range - 1) {
                p->href_progress = j;
            } else {
                p->href_progress = 0;
            }
        }

        if (found_href) {
            // try to consume link
            size_t k = 0;
            if (p->state == SEEK_HREF) {
                // eat =, whitespace
                // size_t k = i + href_len;
                k = i + href_len;
                while (k < max_search_range && isspace(str[k])) k++;
                if (str[k] != '=') continue; // malformed

                k++;
                if (k >= max_search_range) continue;
                while (k < max_search_range && isspace(str[k])) k++;

                char first_char = str[k];
                // fprintf(stderr, "FIRST CHAR: %c\n", first_char);
                char end_char;
                if (first_char == '\'' || first_char == '\"') end_char = first_char;
                else end_char = '>';
                // fprintf(stderr, "end char: %c\n", end_char);
                p->end_char = end_char;
            }

            char* last = memchr(str + k + 1, p->end_char, max_search_range - k);
            
            if (last) {
                // fprintf(stderr, "found last: %c\n", *last);
                i = last - str; // advance index
                char* first = str + k + 1;
                
                size_t link_len = last - first;
                // char* link = (char*) malloc(link_len + 1);
                // memcpy(link, first, link_len);
                // link[link_len] = '\0';
                
                // fprintf(stderr, "found link [%d]: %.*s\n", should_prefetch(first, link_len), link_len, first);

                UriPrefix link_type;
                if ((link_type = should_prefetch(first, link_len)) != PRE_NONE) {
                    char* link = format_link(root_req, first, link_len, link_type);
                    // char* link = malloc(link_len + 1);

                    if (link) {
                        // memcpy(link, first, link_len);
                        // link[link_len] = '\0';
                        fprintf(stderr, "queueing link: %s\n", link);

                        // queue job
                        const char* host = query_str_header(root_req, "Host");
                        pthread_mutex_lock(&pre_lock);
                        if (pre_count < MAX_PREFETCH) {
                            PrefetchJob job = {
                                .uri = link,
                                .host = strdup(host)
                            };
                            pre_stack[pre_count++] = job;
                        }
                        pthread_mutex_unlock(&pre_lock);
                    }
                }

                p->state = SEEK_HREF;                
            } else {
                p->state = CONSUME_LINK;
            }
        }
    }
}

void fetch_and_cache(char* host, char* uri) {
    struct hostent* host_info = gethostbyname(host);
    if (!host_info) {
        return;
    }

    if (is_blocked(host)) {
        return;
    }

    int remote_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (remote_fd < 0) {
        return;
    }

    struct sockaddr_in remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(80);
    memcpy(&remote_addr.sin_addr, host_info->h_addr_list[0], host_info->h_length);

    if (connect(remote_fd, (struct sockaddr*) &remote_addr, sizeof(remote_addr)) < 0) {
        close(remote_fd);
        return;
    }

    // prepend with "/" if necessary
    char canonical_uri[1024];
    if (strstr(uri, "http://") == uri || strstr(uri, "https://") == uri) {
        strncpy(canonical_uri, uri, sizeof(canonical_uri) - 1);
    } else {
        snprintf(canonical_uri, sizeof(canonical_uri), "http://%s/%s", host, uri);
    }

    fprintf(stderr, "[PREFETCH] trying: %s @ %s\n", canonical_uri, host);
    dprintf(
        remote_fd, 
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        canonical_uri,
        host
    );

    try_create_cache();
    char* cache_path = NULL;
    DynamicHttpRequest dummy_req = {0};
    dummy_req.uri = canonical_uri;
    int cached = is_cached(&dummy_req, &cache_path);
    
    if (!cached && remote_fd >= 0) {
        FILE* cache_entry = fopen(cache_path, "w");
        if (!cache_entry) {
            goto pre_cleanup;
        }

        char buf[4096];
        ssize_t n;
        while ((n = recv(remote_fd, buf, sizeof(buf), 0)) > 0) {
            fwrite(buf, sizeof(char), n, cache_entry);
        }

        fprintf(stderr, "[PREFETCH] cache entry created for %s (%s)\n", uri, cache_path);
        if (cache_entry)
            fclose(cache_entry);

    }

pre_cleanup:
    free(cache_path);
    close(remote_fd);
}

void* prefetch_worker(void*) {
    fprintf(stderr, "[PREFETCH] spawning worker\n");

    while (1) {
        PrefetchJob job;
        int has_job = 0;

        pthread_mutex_lock(&pre_lock);
        if (pre_count > 0) {
            job = pre_stack[--pre_count];
            has_job = 1;
        }
        pthread_mutex_unlock(&pre_lock);

        fprintf(stderr, "[PREFETCH] worker has job: %d\n", has_job);
        if (has_job) {
            fetch_and_cache(job.host, job.uri);
            free_job(&job);
        } else {
            // busy loop waiting for jobs
            usleep(1000 * 1000); // 100ms
        }
    }
}

void stream_sockets(DynamicHttpRequest* req, int client_fd, int remote_fd) {
    try_create_cache();
    char* cache_path = NULL;
    int cached = is_cached(req, &cache_path);
    
    // struct pollfd pfd = { .fd = remote_fd, .events = POLLIN | POLLERR | POLLHUP | POLLNVAL };

    if (!cached && remote_fd >= 0) {
        FILE* cache_entry = fopen(cache_path, "w");
        if (!cache_entry) {
            perror("fopen cache entry");
            return;
        }

        HrefParser parser = {0};
        init_href_parser(&parser);

        DynamicHttpRequest resp = { .type = HTTP_RESPONSE };
        Buffer resp_buf = { .len = 0 };

        ssize_t n = read_headers(remote_fd, &resp_buf, PROXY_CONNECTION_TIMEOUT_MS);
        if (n > 0) {
            ssize_t eoh = http_find_eoh(resp_buf.raw, resp_buf.len);
            // long content_length = query_int_header_with_default(&resp, "Content-Length", 0);
            // fprintf(stderr, "RESP CONTENT LENGTH: %ld\n", content_length);
            if (!OK(http_parse_headers(&resp, resp_buf.raw, resp_buf.len))) {
                fprintf(stderr, "could not parse headers, closing\n");
                send_error(client_fd, 500, req->version);
                return;
            }

            // fprintf(stderr, "RESPONSE HEADERS (%ld) -> %zu hdrs\n", (long) n, resp.header_count);
            // for (size_t i = 0; i < resp.header_count; i++) {
            //     fprintf(stderr, "%s: %s\n", resp.headers[i].name, resp.headers[i].value);
            // }

            if (eoh > 0) { // double check
                long content_length = query_int_header_with_default(&resp, "Content-Length", 0);
                size_t header_len = eoh + 4;
                size_t to_stream = header_len + content_length;
                size_t n_sent = 0;

                // fprintf(stderr, "TO_STREAM: %zu (%zu)\n", to_stream, (size_t) content_length);

                // send everything in header + extra
                ssize_t n = resp_buf.len;
                do {
                    send(client_fd, resp_buf.raw, n, 0);
                    fwrite(resp_buf.raw, sizeof(char), n, cache_entry);

                    scrape_links(req, &parser, resp_buf.raw, n);

                    n_sent += n;
                } while (
                    (n = recv(remote_fd, resp_buf.raw, sizeof(resp_buf.raw), 0)) > 0 &&
                    n_sent < to_stream
                );

                // fprintf(stderr, "N_SENT: %zu\n", n_sent);
            }
        }

        if (cache_entry)
            fclose(cache_entry);

        fprintf(stderr, "cache entry should be written\n");
    } else if (cached) {
        fprintf(stderr, "sending from cache: %s\n", req->uri);
        int cache_fd = open(cache_path, O_RDONLY);
        if (cache_fd < 0) return;

        struct stat st;
        if (fstat(cache_fd, &st) < 0) {
            close(cache_fd);
            return;
        }

        off_t offset = 0;
        ssize_t total_sent = 0;
        ssize_t to_send = st.st_size;

        while (to_send > 0) {
            ssize_t sent = sendfile(client_fd, cache_fd, &offset, to_send);
            if (sent <= 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                close(cache_fd);
                return;
            }

            to_send -= sent;
            total_sent += sent;
        }

        close(cache_fd);
    } else {
        fprintf(stderr, "no remote and no cache, closing\n");
        send_error(client_fd, 500, req->version);
    }

    // free the cache entry path string
    if (cache_path) free(cache_path);
}

void* handle_client(void* c_arg) {
    Client* c = (Client*) c_arg;
    Buffer buf = { .len = 0 };

    // initialize connection
    c->proxy_keep_alive = 0;
    printf("initiating new connection: ID %zu\n", c->id);

    while (1) {
        ssize_t header_len = read_headers(c->fd, &buf, PROXY_CONNECTION_TIMEOUT_MS);
        if (header_len <= 0) break;

        DynamicHttpRequest req = {0};
        req.type = HTTP_REQUEST;
        
        if (!OK(http_parse_headers(&req, buf.raw, buf.len))) {
            send_error(c->fd, 400, req.version ? req.version : "HTTP/1.1");
            http_free_request(&req);
            break;
        }

        long content_length = query_int_header_with_default(&req, "Content-Length", 0);
        if (content_length > 0) {
            if (read_body(c->fd, &buf, buf.len + content_length, PROXY_CONNECTION_TIMEOUT_MS) <= 0) {
                http_free_request(&req);
                break;
            }
        }

        // handle keep-alive
        static const char* keep_alive = "keep-alive";
        const char* connection[2] = {
                query_str_header(&req, "Connection"),
                query_str_header(&req, "Proxy-Connection")
        };

        for (size_t i = 0; i < 2; i++) {
            const char* connection_value = connection[i];
            if (!connection_value) continue;

            if (OK_OS(strncasecmp(connection_value, keep_alive, strlen(keep_alive)))) {
                c->proxy_keep_alive = 1;
            }
        }

        // resolve host
        const char* host = query_str_header(&req, "Host");
        if (!host) {
            send_error(c->fd, 404, req.version);
            http_free_request(&req);
            break;
        }

        struct hostent* host_info = gethostbyname(host);
        if (!host_info) {
            send_error(c->fd, 404, req.version);
            http_free_request(&req);
            break;
        }

        if (is_blocked(host)) {
            send_error(c->fd, 403, req.version);
            http_free_request(&req);
            break;
        }

        int remote_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (remote_fd < 0) {
            send_error(c->fd, 500, req.version);
            fprintf(stderr, "could not open socket for remote host\n");
            http_free_request(&req);
            break;
        }

        struct sockaddr_in remote_addr;
        memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(80);
        memcpy(&remote_addr.sin_addr, host_info->h_addr_list[0], host_info->h_length);

        if (connect(remote_fd, (struct sockaddr*) &remote_addr, sizeof(remote_addr)) < 0) {
            // send_error(c->fd, 500, req.version);
            fprintf(stderr, "could not connect to remote host, trying cache\n");
            close(remote_fd);
            // http_free_request(&req);
            // break;

            remote_fd = -1;
        }

        // attempt to send request to server
        send(remote_fd, buf.raw, buf.len, 0);
        stream_sockets(&req, c->fd, remote_fd);

        close(remote_fd);
        http_free_request(&req);

        buf.len = 0;
        if (!c->proxy_keep_alive) {
            fprintf(stderr, "I: keep-alive unspecified, killing connection\n");
            break;
        }
    }

    // free the client
    close(c->fd);
    free(c);

    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage();
        exit(1);
    }

    char* port_str = argv[1];
    long port;

    Result res = str_to_long(port_str, &port);
    if (!OK(res)) {
        fprintf(stderr, "E: invalid port: '%s'\n", port_str);
        exit(1);
    }

    printf("I: attempting to use port: %ld\n", port);

    TTL = 20;
    if (argc >= 3) {
        char* ttl_str = argv[2];
        long ttl_val;
        if (OK(str_to_long(ttl_str, &ttl_val))) {
            TTL = ttl_val;
        }
    }
    fprintf(stderr, "I: using TTL = %ld\n", TTL);

    // use host IP by default for proxy
    struct addrinfo hints, *host_info;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (!OK_OS(getaddrinfo(NULL, port_str, &hints, &host_info)) || !host_info) {
        fprintf(stderr, "E: could not resolve host address\n");
        exit(1);
    }

    // try to create socket
    int lfd = socket(host_info->ai_family, host_info->ai_socktype, host_info->ai_protocol);
    if (lfd < 0) {
        fprintf(stderr, "E: could not bind listening socket\n");
        exit(1);
    }

    // allow port reuse
    int should_reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &should_reuse, sizeof(should_reuse));

    // try to bind socket to host
    if (!OK_OS(bind(lfd, host_info->ai_addr, host_info->ai_addrlen))) {
        fprintf(stderr, "E: could not bind socket to host\n");
        exit(1);
    }

    // try to listen on socket
    if (!OK_OS(listen(lfd, LISTEN_QUEUE_SIZE))) {
        fprintf(stderr, "E: failed to listen at host\n");
        exit(1);
    }

    load_blocklist("./blocklist");

    // create prefetch workers
    for (size_t i = 0; i < 4; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, prefetch_worker, NULL);
        pthread_detach(tid);
    }

    while (1) {
        Client* c = (Client*) malloc(sizeof(Client));
        c->len = sizeof(c->addr);

        if (OK(wait_client(lfd, c))) {
            pthread_t tid;
            if (!OK_OS(pthread_create(&tid, NULL, handle_client, c))) {
                close(c->fd);
                free(c);
            } else {
                pthread_detach(tid);
            }
        } else {
            fprintf(stderr, "W: encountered error trying to connect to client\n");
            free(c);

            // got signal
            if (errno == EINTR) break;
            perror("testing");
        }
    }

    return 0;
}