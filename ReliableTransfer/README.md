I wrote a grading script `grade.py` for PA2 reliable file transfer.

This script builds your project, spins up the server and a single interactive client (via a PTY), runs a series of black-box tests, and prints a score out of 100.

Firstly, you have to put the `grade.py` in the same folder with `udp_server.c, udp_client.c, and Makefile`.

The directory layout should be
```
<your-project-root>/
├── Makefile
├── udp_server.c
├── udp_client.c
├── foo1
├── foo2
├── foo3
└── grade.py
```
The provided Makefile:
  - build `server/udp_server` and `client/udp_client`
  - create `server/` and `client/` if needed
  - copy `foo1, foo2, foo3` into `server/`
    
Tip: the script calls make clean && make automatically.

## What the grader expects your program to support
From the client’s prompt, these commands should be understood by your server:  
  - `ls` — list files available on the server (should include foo1, foo2, foo3)  
  - `get <filename>` — download from server → client  
  - `put <filename>` — upload from client → server  
  - `delete <filename>` — remove from server’s directory  
  - `exit` — gracefully shut down the server process
    
The grader talks only to the client; the server’s `stdout/stderr` are fully suppressed.  

## How the grader runs
  1. Build: `make clean` then `make`
  2. Start server (in background): `cd server && ./udp_server <port>`
  3. Start one client via PTY: `cd client && ./udp_client 127.0.0.1 <port>`
  4. Issue commands to the client (see test list below)  

## Test Cases
| # | Test                        | Points | How it’s graded                                                                                                                                       |
| - | --------------------------- | ------ | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 | `ls`                        | 15     | Client output must contain `foo1`, `foo2`, `foo3`.                                                                                                    |
| 2 | `get foo1`                  | 15     | After \~5s (configurable), SHA-256(**server/foo1**) == SHA-256(**client/foo1**).                                                                      |
| 3 | `delete foo1` → `ls`        | 15     | `foo1` must **not** appear in the listing.                                                                                                            |
| 4 | `put foo1`                  | 15     | After \~5s, SHA-256(**server/foo1**) == SHA-256(**client/foo1**).                                                                                     |
| 5 | `exit`                      | 15     | Server process must exit cleanly (exit code = `0`) within a short timeout.                                                                                              |
| 6 | **Reliability under netem** | 25     | Restart server/client, apply `tc netem delay X loss Y%`, create \~N MB `server/foo2`, `get foo2`, then hashes must match. qdisc is removed afterward. |

## Usage

Depending on your implementation (go-back-N or stop-n-wait) and file size, the wait time for reliable file transfer test would be different. 

You have to experiment with those arguments. 

Also, `netem` is a commandline tool on `Ubuntu`, thus you have to run the script on the course VM or your own Ubuntu machine.

Running the script on other OS won't be able to apply packet loss and latency.

Basic run (defaults shown):
```
python3 grade.py \
  --host 127.0.0.1 \
  --port 8000 \
  --timeout 3.0 \
  --wait 60.0 \
  --delete-wait 1.0 \
  --exit-wait 2.0 \
  --foo2-size-mb 2 \
  --loss 5.0 \
  --delay 1ms
```

Argument reference
  - `--host` / `--port` — server address/port passed to udp_client
  - `--timeout` — how long to capture output for ls tests (seconds)
  - `--wait` — wait time after get/put & reliability fetch before hashing (seconds)
  - `--delete-wait` — short settle time after delete (seconds)
  - `--exit-wait` — how long to wait for the server to exit (seconds)
  - `--foo2-size-mb` — size of server/foo2 for the reliability test (MB). Script builds it by concatenating server/foo1 until ~size.
  - `--loss` — packet loss percentage (e.g., 5%, 10%)
  - `--delay` — latency string for netem (e.g., 1ms, 5ms, 0ms)

## Safety notes for netem
The grader applies:
```
sudo tc qdisc add dev lo root netem delay <DELAY> loss <LOSS>%
```
and removes it with:
```
sudo tc qdisc del dev lo root
```
This affects only the loopback interface `lo`. Still, don’t interrupt the script to avoid leaving qdisc configured. If you do, you can manually clear it:
```
sudo tc qdisc del dev lo root
```
