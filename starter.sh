#!/bin/sh
set -eu

make -f makefile
mkdir -p torrents
rm -f torrents/*.track

WAVE1_CHUNK_DELAY_US=${WAVE1_CHUNK_DELAY_US:-0}
WAVE2_CHUNK_DELAY_US=${WAVE2_CHUNK_DELAY_US:-325000}

TRACKER_PID=""
P1=""
P2=""
WAVE1_PIDS=""
WAVE2_PIDS=""
TAIL_PID=""

cleanup() {
    for pid in $P1 $P2 $WAVE1_PIDS $WAVE2_PIDS $TRACKER_PID $TAIL_PID; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done
}
trap cleanup INT TERM

python3 - <<'PY'
from pathlib import Path
import shutil
for i in range(1, 14):
    root = Path(f"peer{i}")
    for sub in ("shared", "cache", "downloads", "logs"):
        path = root / sub
        if path.exists():
            for child in path.iterdir():
                if child.is_dir():
                    shutil.rmtree(child)
                else:
                    child.unlink()
        path.mkdir(parents=True, exist_ok=True)
    (root / "clientThreadConfig.cfg").write_text("5000\n127.0.0.1\n15\n")
    (root / "serverThreadConfig.cfg").write_text(f"{6000+i}\nshared\n")
(Path("peer1") / "shared" / "small.txt").write_text("This is the small shared file for CS4390.\n")
large = Path("peer2") / "shared" / "large.bin"
with large.open("wb") as f:
    block = b"CS4390 peer to peer demo data.\n" * 128
    for _ in range(512):
        f.write(block)
PY

ALL_LOGS="tracker.log"
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13; do
    : > "peer$i/logs/run.log"
    ALL_LOGS="$ALL_LOGS peer$i/logs/run.log"
done
: > tracker.log

tail -n +1 -f $ALL_LOGS &
TAIL_PID=$!

echo "Wave 1 chunk delay is $WAVE1_CHUNK_DELAY_US microseconds per 1024-byte chunk."
echo "Wave 2 chunk delay is $WAVE2_CHUNK_DELAY_US microseconds per 1024-byte chunk."
echo "Time 0: starting tracker, Peer1, and Peer2."
./tracker sconfig > tracker.log 2>&1 &
TRACKER_PID=$!
sleep 2

./peer Peer1 peer1 --seed > peer1/logs/run.log 2>&1 &
P1=$!
./peer Peer2 peer2 --seed > peer2/logs/run.log 2>&1 &
P2=$!

sleep 30
echo "Time 30 seconds: starting Peer3 through Peer8."
for i in 3 4 5 6 7 8; do
    case "$i" in
        3) START_SEGMENT=0 ;;
        4) START_SEGMENT=8 ;;
        5) START_SEGMENT=16 ;;
        6) START_SEGMENT=24 ;;
        7) START_SEGMENT=4 ;;
        8) START_SEGMENT=12 ;;
    esac
    DEMO_CHUNK_DELAY_US=$WAVE1_CHUNK_DELAY_US PEER_START_SEGMENT=$START_SEGMENT PEER_STAY_ALIVE_AFTER_DOWNLOAD=1 ./peer Peer$i peer$i --download small.txt.track large.bin.track > peer$i/logs/run.log 2>&1 &
    WAVE1_PIDS="$WAVE1_PIDS $!"
done

sleep 60
echo "Time 1 minute 30 seconds: starting Peer9 through Peer13."
for i in 9 10 11 12 13; do
    case "$i" in
        9) START_SEGMENT=0 ;;
        10) START_SEGMENT=7 ;;
        11) START_SEGMENT=14 ;;
        12) START_SEGMENT=21 ;;
        13) START_SEGMENT=28 ;;
    esac
    DEMO_CHUNK_DELAY_US=$WAVE2_CHUNK_DELAY_US PEER_START_SEGMENT=$START_SEGMENT ./peer Peer$i peer$i --download small.txt.track large.bin.track > peer$i/logs/run.log 2>&1 &
    WAVE2_PIDS="$WAVE2_PIDS $!"
done

echo "Time 1 minute 30 seconds: stopping Peer1 and Peer2."
kill "$P1" "$P2" 2>/dev/null || true
wait "$P1" 2>/dev/null || true
wait "$P2" 2>/dev/null || true
echo "Peer1 terminated"
echo "Peer2 terminated"
P1=""
P2=""

for pid in $WAVE2_PIDS; do
    wait "$pid" || true
done
WAVE2_PIDS=""

for pid in $WAVE1_PIDS; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done
WAVE1_PIDS=""

kill "$TRACKER_PID" 2>/dev/null || true
wait "$TRACKER_PID" 2>/dev/null || true
TRACKER_PID=""

kill "$TAIL_PID" 2>/dev/null || true
wait "$TAIL_PID" 2>/dev/null || true
TAIL_PID=""

trap - INT TERM
echo "Demo finished. Check tracker.log and peer*/logs/run.log."
