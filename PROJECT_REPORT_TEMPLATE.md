# CS 4390 Project Report

## Group Members

List members sorted alphabetically by last name.

## Member Roles

- Member 1:
- Member 2:
- Member 3:

## Code Design

The system has two programs:

- `tracker`: a multi-threaded TCP server that stores and serves `.track` files.
- `peer`: a peer program that acts as both a tracker client and a peer upload server.

Shared helper code is in `common.c` and `common.h`.

The tracker supports:

- `REQ LIST`
- `GET filename.track`
- `createtracker`
- `updatetracker`

The peer supports:

- manual testing commands
- automatic seeding
- automatic tracker-file fetch
- multi-threaded chunk download
- chunk upload to other peers
- 1024-byte maximum chunk enforcement
- partial download resume using cache `.parts` files

## Installation Guide

Use Linux or WSL Ubuntu.

```sh
make -f makefile
```

## Running Guide

Manual tracker:

```sh
./tracker sconfig
```

Manual peer:

```sh
./peer Peer1 peer1
```

Automated demo:

```sh
sh starter.sh
```

The regular peer configuration files use a 900-second tracker refresh interval. The starter script lowers this value for the demo run only.

## External Sources

- Socket programming concepts: Beej's Guide to Network Programming, included in the project files.
- MD5 algorithm structure: RFC 1321 / RSA Data Security, Inc. MD5 reference implementation style.
