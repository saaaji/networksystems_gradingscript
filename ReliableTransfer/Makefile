CC     := gcc
CFLAGS := -Wall -Wextra -O2

SERVER_DIR := server
CLIENT_DIR := client

SERVER_BIN := $(SERVER_DIR)/udp_server
CLIENT_BIN := $(CLIENT_DIR)/udp_client

FOO_FILES  := foo1 foo2 foo3

.PHONY: all server client clean

all: server client

server: $(SERVER_BIN)
client: $(CLIENT_BIN)

# Build server binary and copy foo files
$(SERVER_BIN): udp_server.c common.h
	mkdir -p $(SERVER_DIR)
	$(CC) $(CFLAGS) -o $@ $<
	cp -f $(FOO_FILES) $(SERVER_DIR)/

# Build client binary
$(CLIENT_BIN): udp_client.c common.h
	mkdir -p $(CLIENT_DIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(SERVER_DIR) $(CLIENT_DIR)
