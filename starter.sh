#!/bin/sh
set -eu

# Always build first so the demo runs the current source code.
make -f makefile

# The tracker directory must start empty for each demo run.
mkdir -p torrents
rm -f torrents/*.track

# Default to the required 15-minute peer update interval, unless a tester overrides it.
PEER_UPDATE_INTERVAL=${PEER_UPDATE_INTERVAL:-900}
export PEER_UPDATE_INTERVAL

# Process ids are saved so cleanup can stop every background process.
TRACKER_PID=""
P1=""
P2=""
WAVE1_PIDS=""
WAVE2_PIDS=""
PROGRESS_PID=""

# Stop all still-running demo processes if the script is interrupted.
cleanup() {
    for pid in $PROGRESS_PID $P1 $P2 $WAVE1_PIDS $WAVE2_PIDS $TRACKER_PID; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done
}
trap cleanup INT TERM

# Stop a peer and wait for it so the summary is not printed while it is exiting.
stop_peer_pid() {
    pid="$1"
    label="$2"
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        echo "$label terminated"
    fi
}

# Print a live one-line timer during long demo waits.
start_progress() {
    label="$1"
    (
        seconds=0
        while :; do
            seconds=$((seconds + 1))
            printf "\r%s for %d seconds..." "$label" "$seconds"
            sleep 1
        done
    ) &
    PROGRESS_PID=$!
}

# Stop the live timer and move the cursor to the next line.
stop_progress() {
    if [ -n "$PROGRESS_PID" ]; then
        kill "$PROGRESS_PID" 2>/dev/null || true
        wait "$PROGRESS_PID" 2>/dev/null || true
        PROGRESS_PID=""
        printf "\n"
    fi
}

# Sleep with visible progress so the terminal never appears stuck.
progress_sleep() {
    seconds="$1"
    label="$2"
    start_progress "$label"
    sleep "$seconds"
    stop_progress
}

# Reset peer folders/configs and create the two seed files for Peer1 and Peer2.
python3 - <<'PY'
from pathlib import Path
import os
import shutil
peer_update_interval = os.environ.get("PEER_UPDATE_INTERVAL", "900")
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
    (root / "clientThreadConfig.cfg").write_text(f"5000\n127.0.0.1\n{peer_update_interval}\n")
    (root / "serverThreadConfig.cfg").write_text(f"{6000+i}\nshared\n")
(Path("peer1") / "shared" / "small.txt").write_text("This is the small shared file for CS4390.\n")
large = Path("peer2") / "shared" / "large.bin"
with large.open("wb") as f:
    block = b"CS4390 peer to peer demo data.\n" * 128
    for _ in range(512):
        f.write(block)
PY

# Prepare fresh log files for the tracker and every peer.
ALL_LOGS="tracker.log"
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13; do
    : > "peer$i/logs/run.log"
    ALL_LOGS="$ALL_LOGS peer$i/logs/run.log"
done
: > tracker.log

echo "Detailed peer and tracker output is written to tracker.log and peer*/logs/run.log."
echo "The screen output shows milestones, live progress counters, and a final event summary."
echo "Peer update interval is $PEER_UPDATE_INTERVAL seconds."
echo "Seeder mode: Peer1 and Peer2 stay online until validation completes."
echo "Time 0: starting tracker, Peer1, and Peer2."

# Time 0: start the tracker and the two initial seed peers.
./tracker sconfig > tracker.log 2>&1 &
TRACKER_PID=$!
sleep 2

./peer Peer1 peer1 --seed > peer1/logs/run.log 2>&1 &
P1=$!
./peer Peer2 peer2 --seed > peer2/logs/run.log 2>&1 &
P2=$!

# Time 30 seconds: start the first downloader wave.
progress_sleep 30 "Waiting to start Peer3 through Peer8"
echo "Time 30 seconds: starting Peer3 through Peer8."
for i in 3 4 5 6 7 8; do
    ./peer Peer$i peer$i --download small.txt.track large.bin.track --stay > peer$i/logs/run.log 2>&1 &
    WAVE1_PIDS="$WAVE1_PIDS $!"
done

# Time 1 minute 30 seconds: start the second downloader wave.
progress_sleep 60 "Running Peer3 through Peer8 before starting Peer9 through Peer13"
echo "Time 1 minute 30 seconds: starting Peer9 through Peer13."
for i in 9 10 11 12 13; do
    ./peer Peer$i peer$i --download small.txt.track large.bin.track > peer$i/logs/run.log 2>&1 &
    WAVE2_PIDS="$WAVE2_PIDS $!"
done

echo "Time 1 minute 30 seconds: Peer1 and Peer2 remain online for final validation."

# Wait for the second wave to finish; these peers exit by themselves after downloads.
start_progress "Running Peer9 through Peer13"
for pid in $WAVE2_PIDS; do
    wait "$pid" || true
done
stop_progress
WAVE2_PIDS=""

# Wave-1 peers were started with --stay, so stop them after validation.
for pid in $WAVE1_PIDS; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done
WAVE1_PIDS=""

# Stop the original seed peers only after all downloads have been validated.
if [ -n "$P1" ] || [ -n "$P2" ]; then
    echo "Validation complete: stopping Peer1 and Peer2."
    stop_peer_pid "$P1" "Peer1"
    stop_peer_pid "$P2" "Peer2"
    P1=""
    P2=""
fi

# The tracker is no longer needed once all peers have exited.
kill "$TRACKER_PID" 2>/dev/null || true
wait "$TRACKER_PID" 2>/dev/null || true
TRACKER_PID=""

# Print a compact summary instead of flooding the live demo terminal.
echo "Download summary:"
grep -hE "File .*download complete|staying online to serve (downloaded|available) files|failed to download|MD5 check failed|terminated" peer*/logs/run.log || true

echo "Tracker summary:"
grep -hE "tracker: listening|tracker: created|tracker: REQ LIST|tracker: GET" tracker.log | head -40 || true

# Fail the script if any downloader did not complete both required files.
MISSING=0
for i in 3 4 5 6 7 8 9 10 11 12 13; do
    if ! grep -q "Peer$i: File small.txt download complete" "peer$i/logs/run.log" ||
       ! grep -q "Peer$i: File large.bin download complete" "peer$i/logs/run.log"; then
        echo "Peer$i did not complete both downloads."
        MISSING=1
    fi
done

trap - INT TERM
if [ "$MISSING" -ne 0 ]; then
    echo "Demo failed. Check tracker.log and peer*/logs/run.log."
    exit 1
fi
echo "Demo finished. Check tracker.log and peer*/logs/run.log."
