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
make -f makefile clean
make -f makefile
```

Expected result: `gcc` compiles `tracker.c`, `peer.c`, and `common.c` without errors and creates two executables: `tracker` and `peer`.

## Running Guide

### Automated Validation Run

The following commands are the recommended commands for the TA/professor to run from the project directory.

Clean old build artifacts:

```sh
make -f makefile clean
```

Expected result:

```text
rm -f tracker peer *.o
```

Build the project:

```sh
make -f makefile
```

Expected result: several `gcc ...` lines with no compiler errors.

Run the automated validation:

```sh
sh starter.sh
```

Expected result: the script starts the tracker, starts seed peers, starts downloader peers, downloads `small.txt` and `large.bin`, validates completion, and shuts down all processes. The important success lines are:

```text
Peer update interval is 900 seconds.
Seeder mode: Peer1 and Peer2 stay online until validation completes.
Time 0: starting tracker, Peer1, and Peer2.
Time 30 seconds: starting Peer3 through Peer8.
Time 1 minute 30 seconds: starting Peer9 through Peer13.
Peer3: File small.txt download complete
Peer3: File large.bin download complete
...
Peer13: File small.txt download complete
Peer13: File large.bin download complete
Demo finished. Check tracker.log and peer*/logs/run.log.
```

Check the peer logs:

```sh
grep -hE "File .*download complete|failed to download|MD5 check failed" peer*/logs/run.log
```

Expected result: download-complete lines for `Peer3` through `Peer13` and no `failed to download` or `MD5 check failed` lines.

Check tracker events:

```sh
grep -hE "tracker: listening|tracker: created|tracker: GET|updatetracker" tracker.log | head -40
```

Expected result: tracker startup, tracker file creation, `GET`, and `updatetracker` events.

Check downloaded files:

```sh
ls -lh peer3/shared peer9/shared peer13/shared
```

Expected result: each folder contains `small.txt` and `large.bin`.

Clean generated files before submission:

```sh
make -f makefile submit-clean
```

Expected result: compiled binaries, object files, logs, tracker files, cache files, generated shared files, and downloads are removed.

### Manual Run

Manual tracker:

```sh
./tracker sconfig
```

Manual peer:

```sh
./peer Peer1 peer1
```

Non-interactive seed peer:

```sh
./peer Peer1 peer1 --seed
```

Non-interactive downloader:

```sh
./peer Peer3 peer3 --download small.txt.track large.bin.track
```

The regular peer configuration files and `starter.sh` both use a 900-second tracker refresh interval.

### Two-Machine Run

The program is designed to run across two machines. The tracker and peers listen on all network interfaces, and each peer reads the tracker IP, peer port, and advertised peer IP from that peer's configuration files.

On the tracker machine, find the LAN IP:

```sh
hostname -I
```

Expected result: one or more IP addresses. Use the LAN IP reachable from the second machine, for example `192.168.1.25`.

On every peer machine, set line 2 of each `clientThreadConfig.cfg` to the tracker machine IP:

```text
5000
192.168.1.25
900
```

On every peer machine, set line 3 of each `serverThreadConfig.cfg` to either `AUTO` or the reachable IP address of that peer machine:

```text
6003
shared
192.168.1.31
```

Open the required ports between machines:

- tracker port `5000` on the tracker machine
- peer ports `6001` through `6013` for the peers running on each machine

Start the tracker on Machine A:

```sh
./tracker sconfig
```

Start seed peers on the machine that has files in `shared/`:

```sh
./peer Peer1 peer1 --seed
./peer Peer2 peer2 --seed
```

Start downloader peers on either machine:

```sh
./peer Peer3 peer3 --download small.txt.track large.bin.track
```

Expected result: peers on either machine print `File small.txt download complete` and `File large.bin download complete`. If a peer cannot fetch chunks from the other machine, check line 3 of `serverThreadConfig.cfg` and the firewall/WSL networking for that peer port.

## External Sources

- MD5 algorithm structure: RFC 1321 / RSA Data Security, Inc. MD5 reference implementation style. Reference: https://www.rfc-editor.org/rfc/rfc1321
