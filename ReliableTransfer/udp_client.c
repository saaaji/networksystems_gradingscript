/* 
 * udpclient.c - A simple UDP client
 * usage: udpclient <host> <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 

// new headers
#include <time.h>
#include <limits.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <dirent.h>

#define BUFSIZE 1024

/* 
 * error - wrapper for perror
 */
void error(char *msg) {
    perror(msg);
    exit(0);
}


ssize_t SENDTO(int fd, void* data, size_t size, int flags, struct sockaddr* addr, socklen_t addr_len) {
    // float u = (float)rand() / (float)RAND_MAX;
    // if (u < 0.05) {
    //     // fail
    //     // printf("|___ DROPPED!\n");
    //     return 1;
    // }

    return sendto(fd, data, size, flags, addr, addr_len);
}

// #include "common.h"
typedef uint16_t SeqNo;
#define MAX_SEQ_NUM USHRT_MAX
#define HTONSEQ(seq) htons(seq)
#define NTOHSEQ(seq) ntohs(seq)

void inc_seq(SeqNo* seq) {
  if (*seq == MAX_SEQ_NUM) {
    *seq = 0;
  } else {
    (*seq)++;
  }
}

SeqNo dec_seq(SeqNo seq) {
  if (seq == MAX_SEQ_NUM) return 0;
  return seq-1;
}

SeqNo get_next_seq(SeqNo seq) {
  SeqNo next_seq = seq;
  inc_seq(&next_seq);
  return next_seq;
}

#define PAYLOAD_SIZE 1024 * 24 // reliable transfer packet size in bytes
#define FRAME_IS_ACK (1<<0)
#define FRAME_TIMEOUT_MS 50
struct Frame {
  SeqNo seq_num;
  SeqNo ack_num;
  uint8_t flags;
  uint16_t len;
  uint8_t data[PAYLOAD_SIZE];
};

typedef struct ClientHeader {
  size_t cmd_len;
  char cmd_type[16];
  size_t args_len;
  char args[128];
  size_t payload_size;
} ClientHeader;

#define SERVER_SUCCESS (1<<0)
#define RES_IS_FILE (1<<1)
#define RES_IS_STR (1<<2)
#define RES_IS_BYE (1<<3)

#define MAX_GOODBYE_ATTEMPTS 10

typedef struct ServerHeader {
  uint8_t flags; // file, string, goodbye
  size_t payload_size; // size of string, file, etc.
} ServerHeader;

/**
 * FTP config
 */
#define FTP_NUM_COMMANDS 5

struct FTPCmd {
  const char* name;
  int requires_arg;
};

const struct FTPCmd FTP_COMMANDS[FTP_NUM_COMMANDS] = {
  {"get", 1},
  {"put", 1},
  {"delete", 1},
  {"ls", 0},
  {"exit", 0}
};

int main(int argc, char **argv) {
    srand((unsigned int)time(NULL));
  
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    char buf[BUFSIZE];

    /* check command line arguments */
    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
       exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
	  (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    // input loop
    SeqNo current_seq = 0;
    SeqNo nfe = 0;
    struct Frame frame_send;
    struct Frame frame_recv;
    int ack_recv = 1;

    serverlen = sizeof(serveraddr);
    while (1) {
      // get new command into buf
      struct FTPCmd selected_cmd;
      while (1) {
        // recv user input
        bzero(buf, BUFSIZE);
        printf("$ ");
        fgets(buf, BUFSIZE, stdin);

        // ensure null termination
        buf[strcspn(buf, "\n")] = '\0';
        const size_t input_size = strlen(buf);

        // verify syntax
        int valid = 0;
        for (size_t i = 0; i < FTP_NUM_COMMANDS; i++) {
          const struct FTPCmd cmd = FTP_COMMANDS[i];
          const size_t name_size = strlen(cmd.name);
          size_t min_cmd_size = name_size;
          if (cmd.requires_arg) {
            // space + 1 char at least if it takes an argument
            min_cmd_size += 2;
          }

          if (input_size >= min_cmd_size && strncmp(buf, cmd.name, name_size) == 0) {
            valid = 1;
            selected_cmd = cmd;
            break;
          }
        }

        if (valid) break;
      }

      // transmit buf as a command
      ClientHeader hdr = {0};
      strcpy(hdr.cmd_type, selected_cmd.name);
      hdr.cmd_len = strlen(selected_cmd.name);

      if (selected_cmd.requires_arg) {
        hdr.args_len = strlen(buf) - (hdr.cmd_len + 1);
        strcpy(hdr.args, buf + (hdr.cmd_len + 1));
      } else {
        hdr.args_len = 0;
      }
      hdr.payload_size = 0;

      // verify put
      if (strncmp(hdr.cmd_type, "put", hdr.cmd_len) == 0) {
        struct stat st;
        hdr.args[hdr.args_len] = '\0';
        if (stat(hdr.args, &st) == 0) {
          hdr.payload_size = st.st_size;
        } else {
          printf("\trequested file %s doesn't exist, can't send\n");
          continue;
        }
      }

      // send header, wait for ACK
      size_t copy_offset = 0;
      size_t bytes_remaining = sizeof(ClientHeader);
      while (bytes_remaining) {
        printf("%zu remaining on CMD\n", bytes_remaining);
        // send
        size_t chunk_size = bytes_remaining >= PAYLOAD_SIZE ? PAYLOAD_SIZE : bytes_remaining;
        
        frame_send.seq_num = current_seq;
        memcpy(frame_send.data, ((uint8_t*) &hdr) + copy_offset, chunk_size);
        frame_send.flags = 0;
        frame_send.len = chunk_size;

        ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, serverlen);
        if (stat < 0) {
          perror("SENDTO");
        }

        // get ACK
        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, FRAME_TIMEOUT_MS);
        if (ret <= 0) {
          // rentransmit
          continue;
        } else {
          ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, (socklen_t*) &serverlen);
          if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
            if (frame_recv.ack_num != current_seq) {
              continue;
            }
            // otherwise, ACK was received
            printf("GOT ACK FOR #%u\n", frame_recv.ack_num);
          } else {
            if (frame_recv.flags == 0) {
              // send ACK for out of order packet
              frame_send.len = 0;
              frame_send.flags = FRAME_IS_ACK;
              frame_send.seq_num = 0;
              frame_send.ack_num = dec_seq(nfe);

              SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, serverlen);
            }
            continue;
          };
        }

        // increment
        current_seq = get_next_seq(current_seq);
        bytes_remaining -= chunk_size;
        copy_offset += chunk_size;
      }

      // IF PUT, MUST SEND THE WHOLE FILE NOW
      if (hdr.payload_size > 0) {
        hdr.args[hdr.args_len] = '\0';
        FILE* f = fopen(hdr.args, "rb");

        while (1) {
          // send
          int last_packet = 0;
          size_t old_seek = ftell(f);
          frame_send.len = fread(frame_send.data, 1, PAYLOAD_SIZE, f);
          
          printf("sending packet: (%u) %zu\n", current_seq, frame_send.len);

          fseek(f, old_seek, SEEK_SET);
          if (frame_send.len < PAYLOAD_SIZE) last_packet = 1;
          frame_send.seq_num = current_seq;

          ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, serverlen);
          if (stat < 0) {
            perror("SENDTO");
          }

          // get ACK
          struct pollfd pfd;
          pfd.fd = sockfd;
          pfd.events = POLLIN;

          int ret = poll(&pfd, 1, FRAME_TIMEOUT_MS);
          if (ret <= 0) {
            // rentransmit
            continue;
          } else {
            ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, (socklen_t*) &serverlen);
            if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
              if (frame_recv.ack_num != current_seq) {
                continue;
              }
              // otherwise, ACK was received
              printf("got ack for %u\n", current_seq);
            } else {
              if (frame_recv.flags == 0) {
                // send ACK for out of order packet
                frame_send.len = 0;
                frame_send.flags = FRAME_IS_ACK;
                frame_send.seq_num = 0;
                frame_send.ack_num = dec_seq(nfe);

                SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, serverlen);
              }
              continue;
            };
          }

          // increment
          current_seq = get_next_seq(current_seq);

          if (!last_packet)
            fseek(f, old_seek + frame_send.len, SEEK_SET);
          else
            break;
        }
      }

      // wait for server header (1 frame)
      ServerHeader shdr = {0};
      copy_offset = 0;
      
      while (1) {
        ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, (socklen_t*) &serverlen);
        if (stat < 0) {
          perror("recvfrom");
        } else {
          // send ACK
          frame_send.len = 0;
          frame_send.flags = FRAME_IS_ACK;
          frame_send.seq_num = 0;
          frame_send.ack_num = dec_seq(nfe);

          ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, serverlen);
          
          if (stat < 0) {
            perror("SENDTO");
          }

          if (frame_recv.seq_num == nfe) {
            memcpy(&shdr, frame_recv.data, frame_recv.len);
            nfe = get_next_seq(nfe);
            break;
          }
        }
      }

      if (shdr.flags & SERVER_SUCCESS) {
        printf("> request successful\n");
        printf("FLAGS %u (%zu)\n", shdr.flags, shdr.payload_size);
        if (shdr.flags & RES_IS_BYE) {
          printf("quitting...\n");
          exit(0);
        } else if ((shdr.flags & RES_IS_STR) || (shdr.flags & RES_IS_FILE) && shdr.payload_size > 0) {
          // GET STRING FROM SERVER
          if (shdr.flags & RES_IS_FILE)
            printf("GOT FILE: %zu\n", shdr.payload_size);

          // printf("ls size: %zu", shdr.payload_size);

          FILE* output;
          if (shdr.flags & RES_IS_STR) {
            output = stdout;
          } else {
            hdr.args[hdr.args_len] = '\0';
            output = fopen(hdr.args, "wb");
          }

          char* fbuf = NULL;
          if (shdr.payload_size > 0) {
            fbuf = malloc(shdr.payload_size);
          }

          size_t bytes_remaining = shdr.payload_size;
          size_t offset = 0;
          while (bytes_remaining) {
            ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, (socklen_t*) &serverlen);
            if (stat > 0 && frame_recv.flags == 0) {
              if (frame_recv.seq_num == nfe) {
                // append to str
                memcpy(fbuf + offset, frame_recv.data, frame_recv.len);
                // printf("writing segment: (%.*s) %zu\n", frame_recv.len, frame_recv.data, frame_recv.len);
                // fwrite(frame_recv.data, sizeof(char), frame_recv.len, output);


                offset += frame_recv.len;
                bytes_remaining -= frame_recv.len;
                nfe = get_next_seq(frame_recv.seq_num);
              }

              // send ACK
              frame_send.len = 0;
              frame_send.flags = FRAME_IS_ACK;
              frame_send.seq_num = 0;
              frame_send.ack_num = dec_seq(nfe);

              SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &serveraddr, serverlen);
            }
          }

          // fflush(output);
          // if (output != stdout) fclose(output);

          // batch write
          fwrite(fbuf, sizeof(char), shdr.payload_size, output);
          fflush(output);
          if (output != stdout) fclose(output);

          if (fbuf) free(fbuf);
        }
      } else {
        printf("> request failed\n");
      }
      // wait MAX_GOODBYE_ATTEMPTS intervals for data, then quit
    }
}
