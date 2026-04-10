# epoll_demo — Non-blocking Level-triggered Epoll Socket Demo

A minimal but well-structured TCP echo server/client pair that demonstrates
safe, robust use of **epoll** in **non-blocking, level-triggered** mode on Linux.

## Files

| File | Description |
|------|-------------|
| `server.c` | Multi-client echo server driven by `epoll` |
| `client.c` | Client that connects, sends numbered messages, and prints echoed replies |
| `Makefile` | Builds both binaries with strict warnings |

## Key design points

| Feature | How it is implemented |
|---|---|
| Non-blocking I/O | `fcntl(fd, F_SETFL, O_NONBLOCK)` on every socket |
| Level-triggered epoll | No `EPOLLET` flag — default LT mode |
| Graceful shutdown | `SIGINT`/`SIGTERM` set `g_stop`; main loop exits cleanly |
| Broken pipe safety | `SIGPIPE` ignored; `MSG_NOSIGNAL` passed to `send()` |
| Port reuse | `SO_REUSEADDR` + `SO_REUSEPORT` on the listening socket |
| Peer detection | `EPOLLRDHUP` catches half-close; `SO_KEEPALIVE` detects dead peers |
| Non-blocking connect | `connect()` returns `EINPROGRESS`; `EPOLLOUT` + `SO_ERROR` confirm success |

## Build

```bash
cd epoll_demo
make
```

## Run

Open two terminals in the `epoll_demo/` directory.

**Terminal 1 — start the server (default port 9000):**
```bash
./server
# or specify a port:
./server 9001
```

**Terminal 2 — start the client:**
```bash
./client
# or specify host and port:
./client 127.0.0.1 9001
```

The client sends 20 numbered messages at one-second intervals and then exits.
The server echoes every message back and continues accepting new connections
until interrupted with `Ctrl-C`.

## Expected output

**Server:**
```
[server] listening on port 9000
[server] new connection from 127.0.0.1:54321 (fd=5)
[server] echoed 32 byte(s) to fd=5
...
[server] client fd=5 disconnected
^C
[server] shutting down
```

**Client:**
```
[client] connecting to 127.0.0.1:9000 (fd=3)
[client] connected to 127.0.0.1:9000
[client] sent: Hello from client, message #1
[client] received: Hello from client, message #1
...
[client] sent 20 messages, shutting down
```

## Clean up

```bash
make clean
```
