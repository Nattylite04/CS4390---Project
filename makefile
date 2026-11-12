CC = gcc
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -pthread
LIBS = -lcrypto -pthread

.PHONY: all demo clean

all: tracker peer1/peer peer2/peer peer3/peer

tracker: engine/relay_directory.c engine/network_protocol.h
	$(CC) $(CFLAGS) -o $@ engine/relay_directory.c $(LIBS)

peer1/peer: engine/peer_node.c engine/network_protocol.h
	mkdir -p peer1
	$(CC) $(CFLAGS) -o $@ engine/peer_node.c $(LIBS)

peer2/peer: peer1/peer
	mkdir -p peer2
	cp peer1/peer peer2/peer

peer3/peer: peer1/peer
	mkdir -p peer3
	cp peer1/peer peer3/peer

demo: all
	bash run_local_demo.sh

clean:
	rm -f tracker peer1/peer peer2/peer peer3/peer
