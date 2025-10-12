#ifndef COMMON_H

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

ssize_t SENDTO(int fd, void* data, size_t size, int flags, struct sockaddr* addr, socklen_t addr_len) {
    float u = (float)rand() / (float)RAND_MAX;
    if (u < 0.5) {
        // fail
        return 1;
    }

    return sendto(fd, data, size, flags, addr, addr_len);
}

/**
 * GBN config
 */
struct Link {
  int socket_fd;
  struct sockaddr_in addr;
  socklen_t addr_len;
};

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

SeqNo get_next_seq(SeqNo seq) {
  SeqNo next_seq = seq;
  inc_seq(&next_seq);
  return next_seq;
}

// inclusive dist between s1 and s2
size_t seq_dist(SeqNo s1, SeqNo s2) {
  if (s1 <= s2) {
    return s2 - s1 + 1;
  } else {
    return (MAX_SEQ_NUM - s1 + 1) + (s2 + 1);
  }
}

int is_consecutive(SeqNo s1, SeqNo s2) {
  if (s1 == MAX_SEQ_NUM && s2 == 0) {
    return 1;
  } else if (s1 < MAX_SEQ_NUM && s2 == s1 + 1) {
    return 1;
  }
  return 0;
}

// in [w0, w1]?
int in_window(SeqNo w0, SeqNo w1, SeqNo s) {
  if (w0 <= w1) {
    return (s >= w0) && (s <= w1);
  } else {
    return (s >= w0) || (s <= w1);
  }
}

#define PAYLOAD_SIZE 4 // reliable transfer packet size in bytes
#define FRAME_IS_ACK (1<<4)

struct Frame {
  SeqNo seq_num;
  SeqNo ack_num;
  uint8_t flags;
  uint16_t len;
  uint8_t data[PAYLOAD_SIZE];
};

struct SendSlot {
  struct timespec sent_time;
  int acked;
  struct Frame frame;
};

struct RecvSlot {
  int valid;
  struct Frame frame;
};

#define SWS 5 // send-window-size
#define RWS 5 // recv-window-size
#define FRAME_TIMEOUT_MS 1000

#define GBN_BUF_SIZE 1024
struct CircularBuffer {
  uint8_t buffer[GBN_BUF_SIZE];
  size_t head;
  size_t tail;
};

struct GBNP {
  struct Link link;

  // sending
  SeqNo last_ack_recv;
  SeqNo last_frame_sent;
  struct SendSlot send_win[SWS];

  // receiving
  SeqNo largest_acceptable_frame;
  SeqNo last_frame_recv;
  struct RecvSlot recv_win[RWS];

  struct CircularBuffer send_buf;
  struct CircularBuffer recv_buf;
};

void gbn_init(struct GBNP* gbn, int socket_fd, struct sockaddr_in* addr) {
  memset(gbn, 0, sizeof(struct GBNP));
  gbn->link.socket_fd = socket_fd;
  gbn->link.addr = *addr;
  gbn->link.addr_len = sizeof(struct sockaddr_in);

  // nonblocking
  int flags = fcntl(socket_fd, F_GETFL, 0);
  fctnl(socket_fd, F_SETFL, flags | O_NONBLOCK);

  
}

int send_str(struct GBNP* gbn, const char* str) {
  // determine # packets required
  const size_t len = strlen(str) + 1;
  size_t num_packets = len / PAYLOAD_SIZE;
  if (len % PAYLOAD_SIZE != 0) num_packets++;

  size_t transfer_offset = 0;

  printf("sending str: %s (%zu packs)\n", str, num_packets);

  // send full message
  while (transfer_offset < len || gbn->last_ack_recv != gbn->last_frame_sent) {
    // send new frames within window
    while (seq_dist(gbn->last_frame_sent, gbn->last_ack_recv) < SWS && transfer_offset < len) {
      const SeqNo next_seq = get_next_seq(gbn->last_frame_sent);
      struct SendSlot* slot = &gbn->send_win[next_seq % SWS];

      size_t chunk_size = len - transfer_offset;
      if (chunk_size > PAYLOAD_SIZE) chunk_size = PAYLOAD_SIZE;

      memcpy(slot->frame.data, str + transfer_offset, chunk_size);
      slot->frame.len = htons(chunk_size);
      slot->frame.seq_num = HTONSEQ(next_seq);
      slot->frame.ack_num = HTONSEQ(0);
      slot->frame.flags = FRAME_IS_STR;
      if (transfer_offset + chunk_size == len) slot->frame.flags |= FRAME_IS_TAIL;

      slot->acked = 0;
      clock_gettime(CLOCK_MONOTONIC, &slot->sent_time);

      printf("sending FRAME: %u, %u\n", next_seq, chunk_size);

      // send over link
      ssize_t ret = SENDTO(gbn->link.socket_fd, &slot->frame, sizeof(struct Frame), 0, &gbn->link.addr, gbn->link.addr_len);
      if (ret < 0) {
        perror("SENDTO failure");
        continue;
      }
      
      // update LFS
      gbn->last_frame_sent = next_seq;
      transfer_offset += chunk_size;
    }

    // wait for acks
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(gbn->link.socket_fd, &fds);

    struct timeval timeout = {
      .tv_sec = 0,
      .tv_usec = 500 * 1000
    };

    int ret = select(gbn->link.socket_fd + 1, &fds, NULL, NULL, &timeout);
    if (ret > 0 && FD_ISSET(gbn->link.socket_fd, &fds)) {
      struct Frame ack;
      ssize_t ret = recvfrom(gbn->link.socket_fd, &ack, sizeof(ack), 0, (struct sockaddr*) &gbn->link.addr, (socklen_t*) &gbn->link.addr_len);
      if (ret > 0) {
        SeqNo ack_num = NTOHSEQ(ack.ack_num);
        // if within sending window, apply cumulative ACK
        if (in_window(gbn->last_ack_recv + 1, gbn->last_frame_sent, ack_num)) {
          printf("GOT ACK: %u [%u, %u]\n", ack_num, gbn->last_ack_recv+1, gbn->last_frame_sent);

          SeqNo s = gbn->last_ack_recv + 1;
          while (s != get_next_seq(ack_num)) {
            gbn->send_win[s % SWS].acked = 1;
            s = get_next_seq(s);
          }
          gbn->last_ack_recv = ack_num;
        }
      }
    }

    // check timeouts on oldest packet and retransmit 
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    SeqNo oldest = get_next_seq(gbn->last_ack_recv);
    printf("OLDEST %u\n", oldest);
    if (!gbn->send_win[oldest % SWS].acked) {
      double elapsed_ms = (now.tv_sec - gbn->send_win[oldest % SWS].sent_time.tv_sec) * 1000.0 + 
                          (now.tv_nsec - gbn->send_win[oldest % SWS].sent_time.tv_nsec)/1.0e+6;

      if (elapsed_ms > FRAME_TIMEOUT_MS) {
        printf("timeout on seq %u\n", oldest);

        SeqNo s = oldest;
        SeqNo end = get_next_seq(gbn->last_frame_sent);
        while (s != end) {
          struct SendSlot* slot = &gbn->send_win[s % SWS];
          if (!slot->acked) {
            ssize_t ret = SENDTO(gbn->link.socket_fd, &slot->frame, sizeof(struct Frame), 0, &gbn->link.addr, gbn->link.addr_len);
          
            if (ret < 0) {
              perror("SENDTO retransmission failure");
            }

            clock_gettime(CLOCK_MONOTONIC, &slot->sent_time);
          }
          s = get_next_seq(s);
        }
      }
    }
  }

  return 0;
}

int recv_str(struct GBNP* gbn, char* buf, size_t buf_size) {
  size_t transfer_offset = 0;
  while (1) {
    struct Frame frame = {0};
    ssize_t ret = recvfrom(gbn->link.socket_fd, &frame, sizeof(frame), 0, (struct sockaddr*) &gbn->link.addr, (socklen_t*) &gbn->link.addr_len);
    
    if (ret == 0) continue;
    if (ret < 0) {
      error("recvfrom failure");
      continue;
    }

    SeqNo seq_num = NTOHSEQ(frame.seq_num);
    uint16_t payload_len = ntohs(frame.len);

    if (in_window(gbn->last_frame_recv + 1, gbn->largest_acceptable_frame, seq_num)) {
      struct RecvSlot* slot = &gbn->recv_win[seq_num % RWS];
      if (!slot->valid) {
        memcpy(slot->frame.data, frame.data, payload_len);
        slot->frame.len = payload_len;
        slot->valid = 1;
      }
    }

    // advance cumulative ACK
    SeqNo nfe = get_next_seq(gbn->last_frame_recv);
    SeqNo last_recv = gbn->last_frame_recv;
    while (gbn->recv_win[nfe % RWS].valid) {
      struct RecvSlot* slot = &gbn->recv_win[nfe % RWS];
      size_t copy_len = slot->frame.len;

      if (transfer_offset + copy_len > buf_size) copy_len = buf_size - transfer_offset;

      if (copy_len > 0) {
        memcpy(buf + transfer_offset, slot->frame.data, copy_len);
      }
      transfer_offset += copy_len;

      slot->valid = 0;
      last_recv = nfe;
      nfe = get_next_seq(nfe);
    }
    gbn->last_frame_recv = last_recv;

    // update LAF
    gbn->largest_acceptable_frame = gbn->last_frame_recv;
    for (int i = 0; i < RWS; i++) {
      inc_seq(&gbn->largest_acceptable_frame);
    }


    struct Frame ack = {0};
    ack.seq_num = HTONSEQ(0);
    ack.ack_num = HTONSEQ(gbn->last_frame_recv);
    ack.flags = FRAME_IS_ACK;            
    ssize_t ack_ret = SENDTO(gbn->link.socket_fd, &ack, sizeof(struct Frame), 0, &gbn->link.addr, gbn->link.addr_len);
    if (ack_ret < 0) {
      error("SENDTO ACK failure");
      return -1;
    }

    if ((frame.flags & FRAME_IS_TAIL) && gbn->last_frame_recv == seq_num) {
      break;
    }
  }

  return transfer_offset;
}

#endif