/**
 * PACKETIZATION STRATEGY
 * sliding window algorithm:
 * assign sequence number to each frame
 * SENDER: 
 *  SWS (upper bound on un-ACK'd packets)
 *  LAR (last seq. # recv)
 *  LFS (last frame sent)
 *  invariant: LFS - LAR <= SWS
 * RECEIVER:
 *  RWS (upper bound on # of out-of-order frames)
 *  LAF (seq # of largest acceptable frame)
 *  LFR (seq # of last frame received)
 *  invariant: LAF - LFR <= RWS
 */

/* 
 * udpserver.c - A simple UDP echo server 
 * usage: udpserver <port>
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
  exit(1);
}

ssize_t SENDTO(int fd, void* data, size_t size, int flags, struct sockaddr* addr, socklen_t addr_len) {
    // float u = (float)rand() / (float)RAND_MAX;
    // if (u < 0.05) {
    //     // fail
    //     // printf("|___ DROPPED!\n");
    //     return 1;
    // }

    // return sendto(fd, data, size, flags, addr, addr_len);
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

#define PAYLOAD_SIZE 1024 * 32 // reliable transfer packet size in bytes
#define FRAME_IS_ACK (1<<0)
#define FRAME_TIMEOUT_MS 500
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

int main(int argc, char **argv) {
  srand((unsigned int)time(NULL));

  int sockfd; /* socket */
  int portno; /* port to listen on */
  int clientlen; /* byte size of client's address */
  struct sockaddr_in serveraddr; /* server's addr */
  struct sockaddr_in clientaddr; /* client addr */
  struct hostent *hostp; /* client host info */
  char buf[BUFSIZE]; /* message buf */
  char *hostaddrp; /* dotted decimal host addr string */
  int optval; /* flag value for setsockopt */
  int n; /* message byte size */

  /* 
   * check command line arguments 
   */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  portno = atoi(argv[1]);

  /* 
   * socket: create the parent socket 
   */
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) 
    error("ERROR opening socket");

  /* setsockopt: Handy debugging trick that lets 
   * us rerun the server immediately after we kill it; 
   * otherwise we have to wait about 20 secs. 
   * Eliminates "ERROR on binding: Address already in use" error. 
   */
  optval = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
	     (const void *)&optval , sizeof(int));

  /*
   * build the server's Internet address
   */
  bzero((char *) &serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons((unsigned short)portno);

  /* 
   * bind: associate the parent socket with a port 
   */
  if (bind(sockfd, (struct sockaddr *) &serveraddr, 
	   sizeof(serveraddr)) < 0) 
    error("ERROR on binding");

  /* 
   * main loop: wait for a datagram, then echo it
   */
  SeqNo current_seq = 0;
  SeqNo nfe = 0;
  struct Frame frame_send;
  struct Frame frame_recv;

  clientlen = sizeof(clientaddr);
  while (1) {
    // wait for client header first
    ClientHeader hdr = {0};
    size_t copy_offset = 0;

    while (copy_offset < sizeof(ClientHeader)) {
      ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
      
      if (stat > 0 && frame_recv.flags == 0) {
        printf("GOT FRAME #%u (want #%u)\n", frame_recv.seq_num, nfe);

        if (frame_recv.seq_num == nfe) {
          // append to header
          memcpy(((uint8_t*) &hdr) + copy_offset, frame_recv.data, frame_recv.len);

          copy_offset += frame_recv.len;
          nfe = get_next_seq(frame_recv.seq_num);
        }

        // send ACK
        frame_send.len = 0;
        frame_send.flags = FRAME_IS_ACK;
        frame_send.seq_num = 0;
        // frame_send.ack_num = dec_seq(nfe);
        frame_send.ack_num = dec_seq(nfe);

        SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
      }

      // printf("recv: %zu / %zu\n", bytes_received, sizeof(ClientHeader));
    }

    // figure out what to send back
    if (strncmp(hdr.cmd_type, "exit", hdr.cmd_len) == 0) {
      printf("sending goodbye message to client\n");

      // send exit, we can wait N times for an ACK
      int goodbye_attempts = 0;
      ServerHeader shdr = {0};
      shdr.flags = SERVER_SUCCESS | RES_IS_BYE;
      
      // send single shdr frame repeatedly until 
      while (goodbye_attempts++ < MAX_GOODBYE_ATTEMPTS) {
        memcpy(frame_send.data, &shdr, sizeof(ServerHeader));
        frame_send.seq_num = current_seq;
        frame_send.flags = 0;
        frame_send.len = sizeof(ServerHeader);

        ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);

        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, FRAME_TIMEOUT_MS);
        if (ret <= 0) {
          // retransmit
          continue;
        } else {
          ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
          if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
            if (frame_recv.ack_num != current_seq) {
              continue;
            } else {
              // ACK received
              break;
            }
          } else {
            if (frame_recv.flags == 0) {
              // send ACK for out of order packet
              frame_send.len = 0;
              frame_send.flags = FRAME_IS_ACK;
              frame_send.seq_num = 0;
              frame_send.ack_num = dec_seq(nfe);

              SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
            }
            continue;
          };
        }
      }

      // assume connection closed
      current_seq = 0;
      nfe = 0;
      
      // apparently supposed to exit after goodbye
      exit(0);
    } else if (strncmp(hdr.cmd_type, "ls", hdr.cmd_len) == 0) {
      // find total size needed
      size_t ls_size = 0;
      
      DIR* d = opendir(".");
      if (d) {
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL) {
          if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
          }

          ls_size += strlen(entry->d_name) + 1;
        }

        char* ls = malloc(ls_size);
        if (!ls) {
          perror("malloc");
          closedir(d);
          continue;
        }

        rewinddir(d);
        size_t offset = 0;
        while ((entry = readdir(d)) != NULL) {
          if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
          }

          size_t len = strlen(entry->d_name);
          memcpy(ls + offset, entry->d_name, len);
          offset += len;
          ls[offset++] = '\n';
        }

        closedir(d);

        // send header
        ServerHeader shdr = {0};
        shdr.flags = SERVER_SUCCESS | RES_IS_STR;
        shdr.payload_size = ls_size;

        while (1) {
          memcpy(frame_send.data, &shdr, sizeof(ServerHeader));
          frame_send.seq_num = current_seq;
          frame_send.flags = 0;
          frame_send.len = sizeof(ServerHeader);

          ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);

          struct pollfd pfd;
          pfd.fd = sockfd;
          pfd.events = POLLIN;

          int ret = poll(&pfd, 1, FRAME_TIMEOUT_MS);
          if (ret <= 0) {
            // retransmit
            printf("retransmitting\n");
            continue;
          } else {
            ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
            printf("%d\n", (int)stat);
            if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
              if (frame_recv.ack_num != current_seq) {
                continue;
              } else {
                // ACK received
                printf("GOT ACK FOR #%u\n", frame_recv.ack_num);
                break;
              }
            } else {
              if (frame_recv.flags == 0) {
                // send ACK for out of order packet
                frame_send.len = 0;
                frame_send.flags = FRAME_IS_ACK;
                frame_send.seq_num = 0;
                frame_send.ack_num = dec_seq(nfe);

                SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
              }
              continue;
            }
          }
        }
        // increment seq
        current_seq = get_next_seq(current_seq);

        // send ls string
        size_t copy_offset = 0;
        size_t bytes_remaining = ls_size;
        while (copy_offset < ls_size) {
            // send
          size_t chunk_size = bytes_remaining >= PAYLOAD_SIZE ? PAYLOAD_SIZE : bytes_remaining;
          
          frame_send.seq_num = current_seq;
          memcpy(frame_send.data, ls + copy_offset, chunk_size);
          frame_send.flags = 0;
          frame_send.len = chunk_size;

          ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
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
            printf("retransmitting ls DATA: %u\n", current_seq);
            continue;
          } else {
            ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
            if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
              if (frame_recv.ack_num != current_seq) {
                continue;
              }
              printf("GOT ls ACK: #%u\n", frame_recv.ack_num);
              // otherwise, ACK was received
            } else {
                if (frame_recv.flags == 0) {
                // send ACK for out of order packet
                frame_send.len = 0;
                frame_send.flags = FRAME_IS_ACK;
                frame_send.seq_num = 0;
                frame_send.ack_num = dec_seq(nfe);

                SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
              }
              continue;
            };
          }

          // increment
          current_seq = get_next_seq(current_seq);
          bytes_remaining -= chunk_size;
          copy_offset += chunk_size;
        }

        free(ls);
      }
    } else if (strncmp(hdr.cmd_type, "delete", hdr.cmd_len) == 0) {
      hdr.args[hdr.args_len] = '\0';
      int ret = unlink(hdr.args);

      ServerHeader shdr = {0};
      if (ret == 0) shdr.flags = SERVER_SUCCESS;

      while (1) {
        memcpy(frame_send.data, &shdr, sizeof(ServerHeader));
        frame_send.seq_num = current_seq;
        frame_send.flags = 0;
        frame_send.len = sizeof(ServerHeader);

        ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);

        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, FRAME_TIMEOUT_MS);
        if (ret <= 0) {
          // retransmit
          printf("retransmitting\n");
          continue;
        } else {
          ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
          printf("%d\n", (int)stat);
          if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
            if (frame_recv.ack_num != current_seq) {
              continue;
            } else {
              // ACK received
              break;
            }
          } else {
            if (frame_recv.flags == 0) {
              // send ACK for out of order packet
              frame_send.len = 0;
              frame_send.flags = FRAME_IS_ACK;
              frame_send.seq_num = 0;
              frame_send.ack_num = dec_seq(nfe);

              SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
            }
            continue;
          };
        }
      }
      // increment seq
      current_seq = get_next_seq(current_seq);      
    } else if (strncmp(hdr.cmd_type, "get", hdr.cmd_len) == 0) {
      hdr.args[hdr.args_len] = '\0';
      FILE* f = fopen(hdr.args, "rb");

      ServerHeader shdr = {0};
      if (f) {
        shdr.flags = SERVER_SUCCESS;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        shdr.payload_size = size;
        fseek(f, 0, SEEK_SET);
      }
      shdr.flags |= RES_IS_FILE;

      // send header
      while (1) {
        memcpy(frame_send.data, &shdr, sizeof(ServerHeader));
        frame_send.seq_num = current_seq;
        frame_send.flags = 0;
        frame_send.len = sizeof(ServerHeader);

        ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);

        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, FRAME_TIMEOUT_MS);
        if (ret <= 0) {
          // retransmit
          printf("retransmitting\n");
          continue;
        } else {
          ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
          printf("%d\n", (int)stat);
          if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
            if (frame_recv.ack_num != current_seq) {
              continue;
            } else {
              // ACK received
              break;
            }
          } else {
            if (frame_recv.flags == 0) {
              // send ACK for out of order packet
              frame_send.len = 0;
              frame_send.flags = FRAME_IS_ACK;
              frame_send.seq_num = 0;
              frame_send.ack_num = dec_seq(nfe);

              SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
            }
            continue;
          };
        }
      }
      // increment seq
      current_seq = get_next_seq(current_seq);     

      // send actual file!
      while (shdr.payload_size) {
        // send
        int last_packet = 0;
        size_t old_seek = ftell(f);
        frame_send.len = fread(frame_send.data, 1, PAYLOAD_SIZE, f);
        // printf("GOT SEGMENT: %.*s\n", frame_send.len, frame_send.data);
        fseek(f, old_seek, SEEK_SET);
        if (frame_send.len < PAYLOAD_SIZE) last_packet = 1;
        frame_send.seq_num = current_seq;

        ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
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
          ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
          if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
            if (frame_recv.ack_num != current_seq) {
              continue;
            }
            // otherwise, ACK was received
          } else {
            if (frame_recv.flags == 0) {
              // send ACK for out of order packet
              frame_send.len = 0;
              frame_send.flags = FRAME_IS_ACK;
              frame_send.seq_num = 0;
              frame_send.ack_num = dec_seq(nfe);

              SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
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
    } else if (strncmp(hdr.cmd_type, "put", hdr.cmd_len) == 0) {
      // receive the file as payload
      // pipe into new file
      hdr.args[hdr.args_len] = '\0';
      FILE* f = fopen(hdr.args, "wb");

      size_t bytes_remaining = hdr.payload_size;
      while (bytes_remaining) {
        printf("bytes remaining on put: %zu\n", bytes_remaining);
        ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
  
        if (stat > 0 && frame_recv.flags == 0) {
          if (frame_recv.seq_num == nfe) {
            // append to str
            // memcpy(buf, frame_recv.data, frame_recv.len);
            // printf("writing segment: (%.*s) %zu\n", frame_recv.len, frame_recv.data, frame_recv.len);
            fwrite(frame_recv.data, sizeof(char), frame_recv.len, f);

            bytes_remaining -= frame_recv.len;
            nfe = get_next_seq(frame_recv.seq_num);
          }

          // send ACK
          frame_send.len = 0;
          frame_send.flags = FRAME_IS_ACK;
          frame_send.seq_num = 0;
          frame_send.ack_num = dec_seq(nfe);

          SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
        }
      }

      fflush(f);
      fclose(f);

      // SEND RESPONSE HEADER
      ServerHeader shdr = {0};
      shdr.flags = SERVER_SUCCESS;

      while (1) {
        memcpy(frame_send.data, &shdr, sizeof(ServerHeader));
        frame_send.seq_num = current_seq;
        frame_send.flags = 0;
        frame_send.len = sizeof(ServerHeader);

        ssize_t stat = SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);

        struct pollfd pfd;
        pfd.fd = sockfd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, FRAME_TIMEOUT_MS);
        if (ret <= 0) {
          // retransmit
          printf("retransmitting\n");
          continue;
        } else {
          ssize_t stat = recvfrom(sockfd, &frame_recv, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, (socklen_t*) &clientlen);
          printf("%d\n", (int)stat);
          if (stat > 0 && frame_recv.flags & FRAME_IS_ACK) {
            if (frame_recv.ack_num != current_seq) {
              continue;
            } else {
              // ACK received
              break;
            }
          } else {
            if (frame_recv.flags == 0) {
              // send ACK for out of order packet
              frame_send.len = 0;
              frame_send.flags = FRAME_IS_ACK;
              frame_send.seq_num = 0;
              frame_send.ack_num = dec_seq(nfe);

              SENDTO(sockfd, &frame_send, sizeof(struct Frame), 0, (struct sockaddr*) &clientaddr, clientlen);
            }
            continue;
          };
        }
      }
      // increment seq
      current_seq = get_next_seq(current_seq);    
    }
  }
}
