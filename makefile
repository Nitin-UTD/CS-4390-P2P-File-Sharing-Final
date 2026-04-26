CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread

all: tracker peer peer-folders

tracker: tracker.o common.o
	$(CC) $(CFLAGS) -o tracker tracker.o common.o

peer: peer.o common.o
	$(CC) $(CFLAGS) -o peer peer.o common.o

tracker.o: tracker.c common.h
	$(CC) $(CFLAGS) -c tracker.c

peer.o: peer.c common.h
	$(CC) $(CFLAGS) -c peer.c

common.o: common.c common.h
	$(CC) $(CFLAGS) -c common.c

peer-folders: peer
	@for i in 1 2 3 4 5 6 7 8 9 10 11 12 13; do \
		mkdir -p peer$$i/shared peer$$i/cache peer$$i/downloads peer$$i/logs; \
	done

clean:
	rm -f tracker peer *.o

.PHONY: all clean peer-folders
