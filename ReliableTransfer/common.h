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
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>

ssize_t SENDTO(int fd, void* data, size_t size, int flags, struct sockaddr* addr, socklen_t addr_len) {
    float u = (float)rand() / (float)RAND_MAX;
    if (u < 0.5) {
        // fail
        // printf("|___ DROPPED!\n");
        // return 1;
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

#define PAYLOAD_SIZE 128 // reliable transfer packet size in bytes
#define FRAME_IS_ACK (1<<0)
#define FRAME_IS_TAIL (1<<1)

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

#define RING_BUF_SIZE 1024
struct Ring {
  uint8_t buffer[RING_BUF_SIZE];
  size_t head, tail;
};

size_t ring_avail_write(struct Ring* ring) {
  if (ring->tail >= ring->head) {
    return RING_BUF_SIZE - (ring->tail - ring->head) - 1;
  } else {
    return ring->head - ring->tail - 1;
  }
}

size_t ring_used(struct Ring* ring) {
  if (ring->tail >= ring->head) {
    return ring->tail - ring->head;
  } else {
    return RING_BUF_SIZE - ring->head + ring->tail;
  }
}

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

  struct Ring send_buf;
  struct Ring recv_buf;
};

void gbn_init(struct GBNP* gbn, int socket_fd, struct sockaddr_in* addr) {
  memset(gbn, 0, sizeof(struct GBNP));
  gbn->link.socket_fd = socket_fd;
  gbn->link.addr = *addr;
  gbn->link.addr_len = sizeof(struct sockaddr_in);

  gbn->last_ack_recv = MAX_SEQ_NUM;
  gbn->last_frame_sent = MAX_SEQ_NUM;
  gbn->last_frame_recv = MAX_SEQ_NUM;
  
  gbn->largest_acceptable_frame = gbn->last_frame_recv;
  for (int i = 0; i < RWS; i++) {
    inc_seq(&gbn->largest_acceptable_frame);
  }

  int flags = fcntl(socket_fd, F_GETFL, 0);
  fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

  gbn->send_buf.head = 0;
  gbn->send_buf.tail = 0;
  gbn->recv_buf.head = 0;
  gbn->recv_buf.tail = 0;
}

void stream_gbn(struct GBNP* gbn) {
  // SEND NEW FRAMES
  // send new frames within window
  while (seq_dist(gbn->last_ack_recv, gbn->last_frame_sent) <= SWS) {
    // check avail
    size_t avail = ring_used(&gbn->send_buf);
    if (avail == 0) {
      printf("SEND EMPTY\n");
      break;
    }

    const SeqNo next_seq = get_next_seq(gbn->last_frame_sent);
    struct SendSlot* slot = &gbn->send_win[next_seq % SWS];

    size_t chunk_size = avail > PAYLOAD_SIZE ? PAYLOAD_SIZE : avail;
    // copy from ring
    for (size_t i = 0; i < chunk_size; i++) {
      slot->frame.data[i] = gbn->send_buf.buffer[gbn->send_buf.head];
      gbn->send_buf.head = (gbn->send_buf.head + 1) % RING_BUF_SIZE;
    }

    slot->frame.len = htons(chunk_size);
    slot->frame.seq_num = HTONSEQ(next_seq);
    slot->frame.ack_num = HTONSEQ(0);
    slot->frame.flags = 0;
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
  }

  /**
   * RECV LOOP
   */
  while (1) {
    struct Frame frame = {0};
    ssize_t ret = recvfrom(gbn->link.socket_fd, &frame, sizeof(frame), 0, (struct sockaddr*) &gbn->link.addr, (socklen_t*) &gbn->link.addr_len);
    
    if (ret <= 0) {
      // no data
      printf("NO INCOMING, QUTTING\n");
      break;
    }

    if (frame.flags & FRAME_IS_ACK) {
      SeqNo ack_num = NTOHSEQ(frame.ack_num);
      // if within sending window, apply cumulative ACK
      if (in_window(get_next_seq(gbn->last_ack_recv), gbn->last_frame_sent, ack_num)) {
        printf("GOT ACK: %u [%u, %u]\n", ack_num, get_next_seq(gbn->last_ack_recv), gbn->last_frame_sent);

        SeqNo s = get_next_seq(gbn->last_ack_recv);
        while (s != get_next_seq(ack_num)) {
          gbn->send_win[s % SWS].acked = 1;
          s = get_next_seq(s);
        }
        gbn->last_ack_recv = ack_num;
      }
    } else {
      SeqNo seq_num = NTOHSEQ(frame.seq_num);
      uint16_t payload_len = ntohs(frame.len);
      printf("GOT RAW: %u\n", seq_num);

      if (in_window(get_next_seq(gbn->last_frame_recv), gbn->largest_acceptable_frame, seq_num)) {
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

        size_t avail = ring_avail_write(&gbn->recv_buf);
        if (copy_len <= avail) {
          // copy into ringbuf
          for (size_t i = 0; i < copy_len; i++) {
            gbn->recv_buf.buffer[gbn->recv_buf.tail] = slot->frame.data[i];
            gbn->recv_buf.tail = (gbn->recv_buf.tail + 1) % RING_BUF_SIZE;
          }
        } else {
          break; // DON'T ACK ANY MORE THAN THIS
        }

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


      printf("sending ACK for %u\n", gbn->last_frame_recv);
      struct Frame ack = {0};
      ack.seq_num = HTONSEQ(0);
      ack.ack_num = HTONSEQ(gbn->last_frame_recv);
      ack.flags = FRAME_IS_ACK;            
      ssize_t ack_ret = SENDTO(gbn->link.socket_fd, &ack, sizeof(struct Frame), 0, &gbn->link.addr, gbn->link.addr_len);
    }
  }

  // check timeouts on oldest packet and retransmit 
  if (gbn->last_ack_recv != gbn->last_frame_sent) {
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
          // if (!slot->acked) {
          printf("resending %u\n", NTOHSEQ(slot->frame.seq_num));
          ssize_t ret = SENDTO(gbn->link.socket_fd, &slot->frame, sizeof(struct Frame), 0, &gbn->link.addr, gbn->link.addr_len);
        
          if (ret < 0) {
            perror("SENDTO retransmission failure");
          }

          clock_gettime(CLOCK_MONOTONIC, &slot->sent_time);
          // }
          s = get_next_seq(s);
        }
      }
    }
  }
  
  // RECEIVE FRAME
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
    while (seq_dist(gbn->last_ack_recv, gbn->last_frame_sent) <= SWS && transfer_offset < len) {
      const SeqNo next_seq = get_next_seq(gbn->last_frame_sent);
      struct SendSlot* slot = &gbn->send_win[next_seq % SWS];

      size_t chunk_size = len - transfer_offset;
      if (chunk_size > PAYLOAD_SIZE) chunk_size = PAYLOAD_SIZE;

      memcpy(slot->frame.data, str + transfer_offset, chunk_size);
      slot->frame.len = htons(chunk_size);
      slot->frame.seq_num = HTONSEQ(next_seq);
      slot->frame.ack_num = HTONSEQ(0);
      slot->frame.flags = 0;
      if (transfer_offset + chunk_size == len) slot->frame.flags |= FRAME_IS_TAIL;

      slot->acked = 0;
      clock_gettime(CLOCK_MONOTONIC, &slot->sent_time);

      printf("sending FRAME: %u, %u\n", next_seq, chunk_size);

      // send over link
      ssize_t ret = SENDTO(gbn->link.socket_fd, &slot->frame, sizeof(struct Frame), 0, &gbn->link.addr, gbn->link.addr_len);
      if (ret < 0) {
        perror("SENDTO failure");
        break;
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
      if (ret > 0 && ack.flags & FRAME_IS_ACK) {
        SeqNo ack_num = NTOHSEQ(ack.ack_num);
        // if within sending window, apply cumulative ACK
        if (in_window(get_next_seq(gbn->last_ack_recv), gbn->last_frame_sent, ack_num)) {
          printf("GOT ACK: %u [%u, %u]\n", ack_num, get_next_seq(gbn->last_ack_recv), gbn->last_frame_sent);

          SeqNo s = get_next_seq(gbn->last_ack_recv);
          while (s != get_next_seq(ack_num)) {
            gbn->send_win[s % SWS].acked = 1;
            s = get_next_seq(s);
          }
          gbn->last_ack_recv = ack_num;
        }
      }
    }

    // check timeouts on oldest packet and retransmit 
    if (gbn->last_ack_recv != gbn->last_frame_sent) {
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
            // if (!slot->acked) {
            printf("resending %u\n", NTOHSEQ(slot->frame.seq_num));
            ssize_t ret = SENDTO(gbn->link.socket_fd, &slot->frame, sizeof(struct Frame), 0, &gbn->link.addr, gbn->link.addr_len);
          
            if (ret < 0) {
              perror("SENDTO retransmission failure");
            }

            clock_gettime(CLOCK_MONOTONIC, &slot->sent_time);
            // }
            s = get_next_seq(s);
          }
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
    printf("GOT RAW: %u\n", seq_num);

    if (in_window(get_next_seq(gbn->last_frame_recv), gbn->largest_acceptable_frame, seq_num)) {
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


    printf("sending ACK for %u\n", gbn->last_frame_recv);
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

typedef struct DBuf {
  struct Ring send;
  struct Ring recv;
} DBuf;

int send_dbuf(DBuf* dbuf, const void* data, size_t len) {
  if (len == 0) return 0;
  size_t avail = ring_avail_write(&dbuf->send);
  size_t copy_len = (len < avail) ? len : avail;

  for (size_t i = 0; i < copy_len; i++) {
    dbuf->send.buffer[dbuf->send.tail] = ((uint8_t*)data)[i];
    dbuf->send.tail = (dbuf->send.tail + 1) % RING_BUF_SIZE;
  }

  return copy_len;
}

int recv_dbuf(DBuf* dbuf, void* buf, size_t len) {
  if (len == 0) return 0;
  size_t avail = ring_used(&dbuf->recv);
  size_t copy_len = (len < avail) ? len : avail;
  for (size_t i = 0; i < copy_len; i++) {
    ((uint8_t*)buf)[i] = dbuf->recv.buffer[dbuf->recv.head];
    dbuf->recv.head = (dbuf->recv.head + 1) % RING_BUF_SIZE;
  }
  return copy_len;
}

void init_link(struct Link* link, int socket_fd, struct sockaddr_in* addr) {
  link->socket_fd = socket_fd;
  link->addr = *addr;
  link->addr_len = sizeof(struct sockaddr_in);

  int flags = fcntl(socket_fd, F_GETFL, 0);
  fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
}

void init_dbuf(DBuf* dbuf) {
  dbuf->recv.head = 0;
  dbuf->recv.tail = 0;
  dbuf->send.head = 0;
  dbuf->send.tail = 0;
}

void stream_sawp(struct Link* link, DBuf* dbuf) {
  static struct SendSlot slot = {0};
  static SeqNo last_ack_recv = MAX_SEQ_NUM;
  static SeqNo last_frame_sent = MAX_SEQ_NUM;
  static SeqNo last_frame_recv = MAX_SEQ_NUM;
  static int in_flight = 0;

  if (!in_flight) {
    size_t avail = ring_used(&dbuf->send);
    printf("SPACE USED: %zu [%zu, %zu]\n", avail, dbuf->send.head, dbuf->send.tail);
    if (avail > 0) {
      size_t chunk_size = avail > PAYLOAD_SIZE ? PAYLOAD_SIZE : avail;
      memset(&slot.frame, 0, sizeof(slot.frame));

      SeqNo next_seq = get_next_seq(last_frame_sent);
      size_t temp_head = dbuf->send.head;
      for (size_t i = 0; i < chunk_size; i++) {
        slot.frame.data[i] = dbuf->send.buffer[temp_head];
        temp_head = (temp_head + 1) % RING_BUF_SIZE;
      }

      slot.frame.len = htons(chunk_size);
      slot.frame.seq_num = HTONSEQ(next_seq);
      slot.frame.flags = 0;
      slot.acked = 0;

      printf("sending data for [%u, %zu]\n", next_seq, chunk_size);
    
      ssize_t ret = SENDTO(link->socket_fd, &slot.frame, sizeof(slot.frame), 0, &link->addr, link->addr_len);
      if (ret >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &slot.sent_time);

        in_flight = 1;
        last_frame_sent = next_seq;
        dbuf->send.head = temp_head;

        printf("remaining after send: %zu\n", ring_used(&dbuf->send));
      }
    }
  } else {
    // check on timeout
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_ms = (now.tv_sec - slot.sent_time.tv_sec) * 1000.0 + 
                        (now.tv_nsec - slot.sent_time.tv_nsec)/1.0e+6;

    if (elapsed_ms > FRAME_TIMEOUT_MS) {
      ssize_t ret = SENDTO(link->socket_fd, &slot.frame, sizeof(slot.frame), 0, &link->addr, link->addr_len);
      
      if (ret >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &slot.sent_time);
      }
    }
  }

  while (1) {
    struct Frame frame = {0};
    ssize_t ret = recvfrom(link->socket_fd, &frame, sizeof(frame), MSG_DONTWAIT, (struct sockaddr*) &link->addr, &link->addr_len);
    
    if (ret <= 0) {
      break;
    }

    if (frame.flags & FRAME_IS_ACK) {
      SeqNo ack_num = NTOHSEQ(frame.ack_num);
      if (ack_num == last_frame_sent) {
        printf("got ack for %u\n", ack_num);
        last_ack_recv = ack_num;
        in_flight = 0;
      }
    } else {
      SeqNo seq = NTOHSEQ(frame.seq_num);
      uint16_t len = ntohs(frame.len);

      if (seq == get_next_seq(last_frame_recv)) {
        printf("got data for %u\n", seq);
        size_t avail = ring_avail_write(&dbuf->recv);
        
        if (len <= avail) {
          for (size_t i = 0; i < len; i++) {
            dbuf->recv.buffer[dbuf->recv.tail] = frame.data[i];
            dbuf->recv.tail = (dbuf->recv.tail + 1) % RING_BUF_SIZE;
          }

          // send ACK
          struct Frame ack = {0};
          ack.flags = FRAME_IS_ACK;
          ack.ack_num = HTONSEQ(seq);
          printf("sending ack for %u\n", seq);
          SENDTO(link->socket_fd, &ack, sizeof(ack), 0, &link->addr, link->addr_len);
          last_frame_recv = seq;
        }
      } else {
        // out of order
        struct Frame ack = {0};
        ack.flags = FRAME_IS_ACK;
        ack.ack_num = HTONSEQ(last_frame_recv);
        SENDTO(link->socket_fd, &ack, sizeof(ack), 0, &link->addr, link->addr_len);
      }
    }
  }
}

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

#endif