#!/usr/bin/env bash
set -euo pipefail

# Run this on Linux or in Ubuntu/WSL. No changes to your actual peer configs.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

TRACKER_IP=127.0.0.1
TRACKER_PORT="${TRACKER_PORT:-3490}"
PEER1_PORT="${PEER1_PORT:-9101}"
PEER2_PORT="${PEER2_PORT:-9102}"
PEER3_PORT="${PEER3_PORT:-9103}"
TEST_FILE=test_payload_31520.bin
FILE_SIZE=31520

# Each demo has its own temporary tracker, peer folders, and configurations.
WORK="$(mktemp -d)"
LOGS="$ROOT/demo_logs"
mkdir -p "$LOGS"
TRACKER_PID=""
PEER1_PID=""

cleanup() {
    if [[ -n "$PEER1_PID" ]]; then kill "$PEER1_PID" 2>/dev/null || true; fi
    if [[ -n "$TRACKER_PID" ]]; then kill "$TRACKER_PID" 2>/dev/null || true; fi
    # Save the output, even if a test fails, for debugging and screenshots.
    for name in tracker peer1 peer2 peer3; do
        if [[ -f "$WORK/$name/run.log" ]]; then
            cp "$WORK/$name/run.log" "$LOGS/$name.log" || true
        fi
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

printf '\n=== CS4390 P2P local demo ===\n'
echo '[1/6] Compiling tracker and peer programs...'
make all

for name in tracker peer1 peer2 peer3; do
    mkdir -p "$WORK/$name"
done
mkdir -p "$WORK/tracker/torrents"
printf '%s torrents 900\n' "$TRACKER_PORT" > "$WORK/tracker/sconfig"

for idx in 1 2 3; do
    port_var="PEER${idx}_PORT"
    peerport="${!port_var}"
    mkdir -p "$WORK/peer$idx/shared"
    printf '%s %s 60\n' "$TRACKER_PORT" "$TRACKER_IP" > "$WORK/peer$idx/clientThreadConfig.cfg"
    printf '%s shared\n' "$peerport" > "$WORK/peer$idx/serverThreadConfig.cfg"
done

echo "[2/6] Making $FILE_SIZE-byte sample file..."
dd if=/dev/urandom of="$WORK/peer1/shared/$TEST_FILE" bs=1 count="$FILE_SIZE" status=none
ORIGINAL_MD5="$(md5sum "$WORK/peer1/shared/$TEST_FILE" | cut -d ' ' -f 1)"

echo "[3/6] Starting tracker at $TRACKER_IP:$TRACKER_PORT..."
(cd "$WORK/tracker" && exec "$ROOT/tracker" > run.log 2>&1) &
TRACKER_PID=$!
sleep 1
if ! kill -0 "$TRACKER_PID" 2>/dev/null; then
    echo 'ERROR: Tracker did not start. Is port already being used?'
    cat "$WORK/tracker/run.log"
    exit 1
fi

echo "[4/6] Starting peer 1 on port $PEER1_PORT and sharing file..."
(cd "$WORK/peer1" && exec "$ROOT/peer1/peer" serve > run.log 2>&1) &
PEER1_PID=$!
sleep 1
if ! kill -0 "$PEER1_PID" 2>/dev/null; then
    echo 'ERROR: Peer 1 server did not start. Is its port in use?'
    cat "$WORK/peer1/run.log"
    exit 1
fi
(cd "$WORK/peer1" && "$ROOT/peer1/peer" share "$TEST_FILE")

echo '[5/6] Downloading with four worker threads on both peers...'
(cd "$WORK/peer2" && timeout 25 "$ROOT/peer2/peer" get "$TEST_FILE" > run.log 2>&1) &
P2_PID=$!
(cd "$WORK/peer3" && timeout 25 "$ROOT/peer3/peer" get "$TEST_FILE" > run.log 2>&1) &
P3_PID=$!
P2_STATUS=0
P3_STATUS=0
wait "$P2_PID" || P2_STATUS=$?
wait "$P3_PID" || P3_STATUS=$?
if (( P2_STATUS != 0 || P3_STATUS != 0 )); then
    echo "ERROR: Peer downloads failed (peer2=$P2_STATUS, peer3=$P3_STATUS)"
    cat "$WORK/peer2/run.log" "$WORK/peer3/run.log"
    exit 1
fi

P2_MD5="$(md5sum "$WORK/peer2/shared/$TEST_FILE" | cut -d ' ' -f 1)"
P3_MD5="$(md5sum "$WORK/peer3/shared/$TEST_FILE" | cut -d ' ' -f 1)"
echo "Original MD5: $ORIGINAL_MD5"
echo "Peer 2 MD5:  $P2_MD5"
echo "Peer 3 MD5:  $P3_MD5"
if [[ "$ORIGINAL_MD5" != "$P2_MD5" || "$ORIGINAL_MD5" != "$P3_MD5" ]]; then
    echo '[FAIL] MD5 hashes do not match.'
    exit 1
fi
echo '[PASS] Both peers got the same file.'
grep 'worker .* finished' "$WORK/peer2/run.log" "$WORK/peer3/run.log" || true

echo '[6/6] Asking for an invalid 2048-byte chunk (max is 1024)...'
INVALID_RESP="$(python3 - "$PEER1_PORT" "$TEST_FILE" <<'PY'
import socket
import sys

port = int(sys.argv[1])
filename = sys.argv[2]
with socket.create_connection(('127.0.0.1', port), timeout=4) as sock:
    sock.settimeout(4)
    sock.sendall(f'<GET {filename} 0 2048>\n'.encode())
    print(sock.recv(256).decode().strip())
PY
)"
echo "Peer 1 replied: $INVALID_RESP"
if [[ "$INVALID_RESP" != '<GET invalid>' ]]; then
    echo '[FAIL] Invalid chunk request was not rejected properly.'
    exit 1
fi

echo '[PASS] Oversized request rejected correctly.'
echo '=== ALL LOCAL DEMO CHECKS PASSED ==='
echo "Log files are saved to: $LOGS"
echo 'NOTE: This is one computer with 3 local peers, NOT a two-computer demonstration.'
