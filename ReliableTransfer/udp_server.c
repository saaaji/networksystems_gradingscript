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

#define BUFSIZE 1024

/*
 * error - wrapper for perror
 */
void error(char *msg) {
  perror(msg);
  exit(1);
}

#include "common.h"

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
  // clientlen = sizeof(clientaddr);
  struct Link link = {
    .socket_fd = sockfd,
    .addr = clientaddr,
    .addr_len = sizeof(struct sockaddr_in)
  };

  struct GBNP gbn;
  gbn.link = link;
  gbn.last_ack_recv = MAX_SEQ_NUM;
  gbn.last_frame_sent = MAX_SEQ_NUM;
  gbn.last_frame_recv = MAX_SEQ_NUM;
  gbn.largest_acceptable_frame = gbn.last_frame_recv + RWS;
  memset(gbn.send_win, 0, sizeof(gbn.send_win));
  memset(gbn.recv_win, 0, sizeof(gbn.recv_win));

  while (1) {
    int n = recv_str(&gbn, buf, BUFSIZE);
    if (n < 0) {
      error("recv_str failure");
      continue;
    }

    printf("got complete message: %s\n", buf);

    // sleep(100);

    n = send_str(&gbn, buf);
    if (n < 0) {
      error("send_str failure");
      continue;
    }

    /*
     * recvfrom: receive a UDP datagram from a client
     */
    // bzero(buf, BUFSIZE);
    // n = recvfrom(sockfd, buf, BUFSIZE, 0,
		//  (struct sockaddr *) &clientaddr, &clientlen);
    // if (n < 0)
    //   error("ERROR in recvfrom");

    /* 
     * gethostbyaddr: determine who sent the datagram
     */
    // hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, 
		// 	  sizeof(clientaddr.sin_addr.s_addr), AF_INET);
    // if (hostp == NULL)
    //   error("ERROR on gethostbyaddr");
    // hostaddrp = inet_ntoa(clientaddr.sin_addr);
    // if (hostaddrp == NULL)
    //   error("ERROR on inet_ntoa\n");
    // printf("server received datagram from %s (%s)\n", 
	  //  hostp->h_name, hostaddrp);
    // printf("server received %d/%d bytes: %s\n", strlen(buf), n, buf);
    
    /* 
     * sendto: echo the input back to the client 
     */
    // n = sendto(sockfd, buf, strlen(buf), 0, 
	  //      (struct sockaddr *) &clientaddr, clientlen);
    // if (n < 0) 
    //   error("ERROR in sendto");
  }
}
