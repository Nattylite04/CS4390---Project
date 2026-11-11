# CS4390---Project

Hey guys, I've finished writing the main project code and organizing the folder structure. I've also pushed the code to our GitHub.

**What I've completed:**
- Created the tracker server code (`relay_directory.c`).
- Created the peer communication and file transfer code (`peer_node.c`).
- Created the shared networking functions (`network_protocol.h`).
- Implemented multithreading in the tracker and peer downloader.
- Created the Makefile for compiling the project.
- Organized the project into `engine`, `peer1`, `peer2`, `peer3`, and `torrents` folders.

**What still needs to be done for the November 11 midterm:**

1. **Compile and test the code.** Make sure the tracker and peer programs run correctly. Fix any compilation or runtime errors and complete any configuration needed to run them.

2. **Verify multithreading.** Make sure the implementation actually runs multiple threads and capture screenshots showing the relevant code and execution.

3. **Demonstrate file transfer between two separate computers.** Make sure one machine can send a file to another successfully. Take screenshots showing the successful transfer.

4. **Complete the 1–2 page midterm report.** The report must include:
   - What we've completed so far.
   - What still needs to be completed.
   - Each group member's actual contributions.
   - Group member names sorted alphabetically by last name.
   - Screenshots of the code and working implementation.

5. **Submit the report through Canvas by November 11.**

I handled the main code implementation and project structure. The code has not been fully validated yet, so the next priority is testing, debugging, and preparing the two-machine demonstration.

We don't need to finish the entire final project for this midterm. Let's focus on demonstrating multithreading and file transfer successfully.

GitHub: https://github.com/Nattylite04/CS4390---Project

---

## Tracker server update (Muhammad Waleedullah)

I took over the tracker server (`engine/relay_directory.c`) and got it tested and finished for the midterm.

**What I tested:** compiled it with the makefile (no warnings) and tested every command by hand with `nc`:
- `createtracker` returns `succ`, returns `ferr` if the tracker file is already there, and returns `fail` on a bad message
- `updatetracker` returns `succ`, returns `ferr` if there's no tracker file, and returns `fail` on a bad message
- `REQ LIST` returns the `<REP LIST X>` ... `<REP LIST END>` format
- `GET file.track` returns `<REP GET BEGIN>` ... `<REP GET END md5>`

I also ran the whole thing with the real peer program: peer 1 shared a 31,520 byte file and peer 2 downloaded it with 4 threads, and the MD5 matched.

**What I added / fixed:**
- **Dead peer removal.** The spec says the tracker has to drop peers that stop updating. When an `updatetracker` comes in, the tracker now removes any peer line whose timestamp is older than the timeout.
- **Timeout in the config.** `sconfig` now takes an optional 3rd value, the timeout in seconds (`3490 torrents 900`). If it's missing it defaults to 900 sec (15 min), same as the peer refresh default.
- **Error message fix.** A bad `updatetracker` used to answer `<updatetracker invalid fail>`. It now answers `<updatetracker filename fail>` like the spec says.
- **Event logging.** Every request now prints which peer (ip:port) sent it and which thread is handling it, plus a line for LIST, for GET on a missing file, and for each dead peer removed. This helps for the demo screenshots, since you can see different threads serving different peers.

**Multithreading check:** sent 20 LIST requests at the same time and all 20 were answered. The log shows them handled by different worker threads (`pthread_create` + `pthread_detach` for each connection, with one mutex around the .track files).
