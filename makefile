
.RECIPEPREFIX := >

CC = gcc
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -pthread
LIBS = -lcrypto -pthread

all: tracker peer1/peer peer2/peer peer3/peer

tracker: engine/relay_directory.c engine/network_protocol.h
>$(CC) $(CFLAGS) -o $@ engine/relay_directory.c $(LIBS)

peer1/peer: engine/peer_node.c engine/network_protocol.h
>$(CC) $(CFLAGS) -o $@ engine/peer_node.c $(LIBS)

peer2/peer: peer1/peer
>cp peer1/peer peer2/peer

peer3/peer: peer1/peer
>cp peer1/peer peer3/peer

clean:
>rm -f tracker peer1/peer peer2/peer peer3/peer
