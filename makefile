CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread

# Build both required executables and create peer folders for manual testing.
all: tracker peer peer-folders

# Tracker links tracker-specific code with shared helpers.
tracker: tracker.o common.o
	$(CC) $(CFLAGS) -o tracker tracker.o common.o

# Peer links peer-specific code with shared helpers.
peer: peer.o common.o
	$(CC) $(CFLAGS) -o peer peer.o common.o

# Object-file rules keep compilation warnings visible for each source file.
tracker.o: tracker.c common.h
	$(CC) $(CFLAGS) -c tracker.c

peer.o: peer.c common.h
	$(CC) $(CFLAGS) -c peer.c

common.o: common.c common.h
	$(CC) $(CFLAGS) -c common.c

# Create the simulated-peer directories used by the demo and manual runs.
peer-folders: peer
	@for i in 1 2 3 4 5 6 7 8 9 10 11 12 13; do \
		mkdir -p peer$$i/shared peer$$i/cache peer$$i/downloads peer$$i/logs; \
	done

# Remove compiled outputs; required before final submission.
clean:
	rm -f tracker peer *.o

# Remove generated runtime state so the folder is safe to zip for submission.
submit-clean: clean
	rm -f tracker.log
	rm -f torrents/*.track
	rm -f peer*/logs/*.log
	rm -f peer*/cache/*.track peer*/cache/*.parts
	rm -f peer*/shared/* peer*/downloads/*

.PHONY: all clean submit-clean peer-folders
