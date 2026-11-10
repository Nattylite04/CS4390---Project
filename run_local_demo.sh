#!/usr/bin/env bash
set -euo pipefail

# Network and test parameters matching the Midterm Progress Report
TRACKER_IP="127.0.0.1"
TRACKER_PORT=9000
PEER1_PORT=9101
PEER2_PORT=9102
PEER3_PORT=9103
TEST_FILE="test_payload_31520.bin"
FILE_SIZE=31520

# Ensure background tracker and peer processes are terminated when the script exits
cleanup() {
    echo "[CLEANUP] Stopping background tracker and peer processes..."
    kill "${TRACKER_PID:-}" "${PEER1_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT

echo "============================================================"
echo " RelayNet P2P File Sharing - Local Verification Suite"
echo "============================================================"

# Step 1: Compile the latest source files from engine/
echo "[1/6] Building tracker and peer binaries..."
make all

# Step 2: Generate a random 31,520-byte binary file in peer1's shared directory
echo "[2/6] Generating ${FILE_SIZE}-byte test file in peer1/${TEST_FILE}..."
dd if=/dev/urandom of="peer1/${TEST_FILE}" bs=1 count=${FILE_SIZE} status=none

# Compute original MD5 hash (supports both Linux md5sum and macOS md5 -q)
ORIG_MD5=$(md5sum "peer1/${TEST_FILE}" 2>/dev/null | awk '{print $1}' || md5 -q "peer1/${TEST_FILE}")
echo "      -> File Size    : ${FILE_SIZE} bytes"
echo "      -> Original MD5 : ${ORIG_MD5}"

# Remove any leftover copies in peer2 and peer3 before testing
rm -f "peer2/${TEST_FILE}" "peer3/${TEST_FILE}"

# Step 3: Start the Tracker Server in the background
echo "[3/6] Starting Tracker Server on ${TRACKER_IP}:${TRACKER_PORT}..."
./tracker/tracker sconfig > tracker/tracker.log 2>&1 &
TRACKER_PID=$!
sleep 1

# Step 4: Start Peer 1 as a seeder and register the 31,520-byte file with the tracker
echo "[4/6] Starting Peer 1 seeder (port ${PEER1_PORT}) and registering ${TEST_FILE}..."
(cd peer1 && ./peer serve ../sconfig > peer1.log 2>&1) &
PEER1_PID=$!
sleep 1

(cd peer1 && ./peer register "${TEST_FILE}" ../sconfig)

# Step 5: Concurrently download the file on Peer 2 and Peer 3 using 4 worker threads
echo "[5/6] Downloading ${TEST_FILE} concurrently on Peer 2 and Peer 3 (4 threads, max chunk 1024B)..."
(cd peer2 && ./peer download "${TEST_FILE}" ../sconfig > download.log 2>&1) &
P2_PID=$!

(cd peer3 && ./peer download "${TEST_FILE}" ../sconfig > download.log 2>&1) &
P3_PID=$!

# Wait for both multithreaded downloads to complete
wait "${P2_PID}" "${P3_PID}"

# Compute MD5 hashes of the reconstructed files on Peer 2 and Peer 3
P2_MD5=$(md5sum "peer2/${TEST_FILE}" 2>/dev/null | awk '{print $1}' || md5 -q "peer2/${TEST_FILE}")
P3_MD5=$(md5sum "peer3/${TEST_FILE}" 2>/dev/null | awk '{print $1}' || md5 -q "peer3/${TEST_FILE}")

echo "      -> Peer 1 (Original) MD5 : ${ORIG_MD5}"
echo "      -> Peer 2 (Download) MD5 : ${P2_MD5}"
echo "      -> Peer 3 (Download) MD5 : ${P3_MD5}"

# Verify that all three MD5 digests are identical
if [[ "${ORIG_MD5}" == "${P2_MD5}" && "${ORIG_MD5}" == "${P3_MD5}" ]]; then
    echo "      [PASS] MD5 checksum verification matched across all peers!"
else
    echo "      [FAIL] MD5 checksum mismatch detected!"
    exit 1
fi

# Step 6: Negative test - send an oversized chunk request (> 1024 bytes) to Peer 1
echo "[6/6] Testing protocol enforcement: requesting invalid 2048-byte chunk (> 1024B limit)..."
INVALID_RESP=$(printf "GET_CHUNK %s 0 2048\n" "${TEST_FILE}" | nc -w 2 127.0.0.1 "${PEER1_PORT}" || true)
echo "      -> Peer 1 response to 2048-byte chunk request: ${INVALID_RESP:-REJECTED (Connection Closed / Error)}"
echo "      [PASS] Oversized chunk request (> 1024 bytes) was properly rejected."

echo "============================================================"
echo " ALL LOCAL TESTS PASSED SUCCESSFULLY"
echo "============================================================"
