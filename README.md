# CS 4390 P2P File Sharing Project

This project implements a TCP-based peer-to-peer file sharing system with a centralized tracker server.

## Build

Use Linux or WSL Ubuntu:

```sh
make -f makefile
```

This builds:

- `tracker`
- `peer`

Do not submit those compiled binaries or `.o` files.

## Configuration

Tracker config is `sconfig`:

```text
5000
torrents
1800
```

The lines are:

1. tracker port
2. tracker-file directory
3. dead-peer timeout in seconds

Each peer folder has:

- `clientThreadConfig.cfg`
- `serverThreadConfig.cfg`
- `shared/`
- `cache/`
- `downloads/`
- `logs/`

`clientThreadConfig.cfg`:

```text
tracker-port
tracker-ip
periodic-update-interval-seconds
```

The peer config files use `900` seconds, which is the required 15-minute default. `starter.sh` also uses `900` seconds by default and keeps the original seed peers online until validation completes.

`serverThreadConfig.cfg`:

```text
peer-listen-port
shared-folder-name
```

## Manual Run

Start the tracker:

```sh
./tracker sconfig
```

Start an interactive peer:

```sh
./peer Peer1 peer1
```

Start a non-interactive seed or downloader:

```sh
./peer Peer1 peer1 --seed
./peer Peer3 peer3 --download small.txt.track large.bin.track
./peer Peer3 peer3 --download small.txt.track large.bin.track --stay
```

Manual peer commands:

```text
LIST
GET filename.track
createtracker filename description
createtracker filename filesize description md5 ip-address port-number
updatetracker filename start_byte end_byte
updatetracker filename start_byte end_byte ip-address port-number
quit
```

## Automated Demo

Run:

```sh
sh starter.sh
```

The script:

1. builds the project
2. starts the tracker
3. starts `Peer1` and `Peer2` as seed peers
4. starts `Peer3` through `Peer8` after 30 seconds
5. starts `Peer9` through `Peer13` after 1 minute 30 seconds
6. validates that `Peer3` through `Peer13` download both files
7. stops all running peers and the tracker

Logs are written to:

```text
tracker.log
peer*/logs/run.log
```

## Protocols

Tracker commands follow the project handout:

```text
<REQ LIST>
<GET filename.track>
<createtracker filename filesize description md5 ip-address port-number>
<updatetracker filename start_byte end_byte ip-address port-number>
```

Peer-to-peer chunk transfer uses this internal command:

```text
<GETCHUNK filename start_byte end_byte>
```

The peer server enforces a maximum chunk size of `1024` bytes and replies with:

```text
<GET invalid>
```

when the request is invalid.

## External Code Notice

The MD5 implementation in `common.c` follows the standard RSA Data Security, Inc. MD5 algorithm structure from RFC 1321-style public reference implementations.

Reference: RFC 1321, The MD5 Message-Digest Algorithm, https://www.rfc-editor.org/rfc/rfc1321
