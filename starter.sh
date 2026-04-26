#!/bin/sh
set -eu

make -f makefile
mkdir -p torrents
rm -f torrents/*.track

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

./tracker sconfig > tracker.log 2>&1 &
TRACKER_PID=$!
sleep 2

./peer Peer1 peer1 --seed > peer1/logs/run.log 2>&1 &
P1=$!
./peer Peer2 peer2 --seed > peer2/logs/run.log 2>&1 &
P2=$!

sleep 30
DOWNLOAD_PIDS=""
for i in 3 4 5 6 7 8; do
    ./peer Peer$i peer$i --download small.txt.track large.bin.track > peer$i/logs/run.log 2>&1 &
    DOWNLOAD_PIDS="$DOWNLOAD_PIDS $!"
done

sleep 60
for i in 9 10 11 12 13; do
    ./peer Peer$i peer$i --download small.txt.track large.bin.track > peer$i/logs/run.log 2>&1 &
    DOWNLOAD_PIDS="$DOWNLOAD_PIDS $!"
done

for pid in $DOWNLOAD_PIDS; do
    wait "$pid" || true
done

kill "$P1" "$P2" 2>/dev/null || true
echo "Peer1 terminated"
echo "Peer2 terminated"

kill "$TRACKER_PID" 2>/dev/null || true
echo "Demo finished. Check tracker.log and peer*/logs/run.log."
