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

#define BUFSIZE 1024

/* 
 * error - wrapper for perror
 */
void error(char *msg) {
    perror(msg);
    exit(0);
}

#include "common.h"

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

    /**
     * CLIENT LOOP
     */
    struct Link link = {
      .socket_fd = sockfd,
      .addr = serveraddr,
      .addr_len = sizeof(serveraddr)
    };

    // init GBN state
    struct GBNP gbn;
    gbn.link = link;
    gbn.last_ack_recv = MAX_SEQ_NUM;
    gbn.last_frame_sent = MAX_SEQ_NUM;
    gbn.last_frame_recv = MAX_SEQ_NUM;
    gbn.largest_acceptable_frame = gbn.last_frame_recv + RWS;
    memset(gbn.send_win, 0, sizeof(gbn.send_win));
    memset(gbn.recv_win, 0, sizeof(gbn.recv_win));

    // input loop
    while (1) {
      // recv user input
      bzero(buf, BUFSIZE);
      printf("> ");
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
          break;
        }
      }
      
      if (valid) {
        printf("trying send\n");
        send_str(&gbn, buf);
      } else {
        printf("invalid request '%s'\n", buf);
        continue;
      }

      recv_str(&gbn, buf, BUFSIZE);
      printf("response: %s\n", buf);
    }
}
