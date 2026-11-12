# CS4390 local demo: Windows + WSL

These are replacement files for the existing **Git-cloned** repository, not a second project. Copy `makefile`, `run_local_demo.sh`, `.gitattributes`, and `.gitignore` into the cloned repository root (`CS4390---Project`) and replace the old makefile and script. **Do not delete or replace `.git`**. You do not need to copy the `engine/` source files; they were not changed for this fix.

## 1. Install or open Ubuntu/WSL

In PowerShell, check with `wsl --list --verbose`. If Ubuntu/WSL is not installed, use `wsl --install -d Ubuntu` and follow the Windows prompts, including any requested reboot. Open an Ubuntu terminal.

## 2. Build and run the local demo

```bash
cd /mnt/c/Users/curti/Downloads/CS4390---Project
sudo apt update
sudo apt install -y build-essential libssl-dev python3
make clean
make demo
```

You should see `ALL LOCAL DEMO CHECKS PASSED` near the bottom. The script automatically compiles both programs, starts a tracker (port 3490) and peer server (port 9101), registers a random file, downloads it on peer 2 and peer 3 using four threads each, compares three MD5 checksums, and checks the response to an invalid 2048-byte chunk request.

You can use `make` to compile without running a demo, or `make clean` to remove generated binaries.

## 3. Open the test logs

From the root of the project:

```bash
cat demo_logs/tracker.log
cat demo_logs/peer1.log
cat demo_logs/peer2.log
cat demo_logs/peer3.log
```

The script uses temporary peer configurations and temporary data files, so it does not overwrite real files in `peer1/`, `peer2/`, `peer3/`, or `torrents/`. The `demo_logs/` folder is ignored by Git by the supplied `.gitignore`.

## 4. If it fails

- **`make: command not found`**: install `build-essential` in Ubuntu/WSL.
- **`fatal error: openssl/evp.h: No such file or directory`**: install `libssl-dev`.
- **`tracker listen: Address already in use`**: another app is using port 3490. Run `TRACKER_PORT=12090 make demo`, or stop the other server.
- **`peer listen: Address already in use`**: change the peer ports, e.g. `PEER1_PORT=12101 PEER2_PORT=12102 PEER3_PORT=12103 make demo`.
- **`python3: command not found`**: install `python3`.
- **`permission denied` / Windows execution issue**: invoke `make demo` from the **Ubuntu/WSL terminal**, not PowerShell. The Makefile uses `bash run_local_demo.sh` and does not require the executable bit to be set on the script.

## 5. Git workflow (after replacing the files)

```bash
git status
git diff -- makefile run_local_demo.sh
git add makefile run_local_demo.sh .gitattributes .gitignore
git commit -m "Fix build and local P2P demo"
```

Push changes only if your teammates agree and you have write access; otherwise send them the fixed files or open a pull request.

## Scope

This checks only **one computer** with three local peers. The midterm assignment also calls for a file-transfer demonstration between **two separate computers**. That needs separate real network/IP and firewall configuration, and you should capture your own screenshots of that test.
