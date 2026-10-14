# penent

A tiny, dependency-free C wrapper that gives you **one event-loop API** on top of:

- **epoll** on Linux
- **kqueue** on macOS, FreeBSD, OpenBSD and NetBSD

Write your non-blocking server once, compile it anywhere.

The whole library is two files: `penent.h` and `penent.c`.

## Features

- Single, minimal API for both backends (selected at compile time)
- Non-blocking `accept`, `read` and `write` helpers
- Accepted sockets are automatically set to non-blocking and registered for read events
- Normalized event info (`ident`, `filter`, `flags`) so your loop doesn't need `#ifdef`s
- Consistent return codes (`-1` error, `-2` would block)

## Installation

Copy `penent.h` and `penent.c` into your project and compile them with the rest of your sources:

```sh
cc -Wall -Wextra -O2 -o server main.c penent.c
```

No extra libraries or flags are needed on any supported platform.

## Quick start

A minimal echo server:

```c
#include "penent.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 64

int main(void) {
  /* 1. Create a non-blocking listening socket */
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(8080);

  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(server_fd, 128) < 0) {
    perror("bind/listen");
    return 1;
  }
  fcntl(server_fd, F_SETFL, fcntl(server_fd, F_GETFL, 0) | O_NONBLOCK);

  /* 2. Create the event queue and watch the listening socket */
  int pn_fd = pn_init();
  if (pn_fd < 0 || pn_add_event(pn_fd, server_fd, PN_FILT_READ) < 0) {
    perror("penent init");
    return 1;
  }

  penent_event_t events[MAX_EVENTS];
  char buf[4096];

  /* 3. Event loop */
  for (;;) {
    int n = pn_monitor(pn_fd, events, MAX_EVENTS, -1); /* -1 = block forever */
    if (n < 0)
      continue;

    for (int i = 0; i < n; i++) {
      penent_stats_t ev = pn_get_event(events[i]);

      if (ev.ident == server_fd) {
        pn_accept(pn_fd, server_fd); /* new client, already registered */
        continue;
      }

      if (ev.flags == PN_ERR) {
        pn_del_event(pn_fd, ev.ident, PN_FILT_READ);
        close(ev.ident);
        continue;
      }

      if (ev.filter == PN_FILT_READ) {
        int r = pn_read(ev.ident, buf, sizeof(buf), pn_fd);
        if (r > 0)
          pn_write(ev.ident, buf, (size_t)r, pn_fd); /* echo back */
        /* r == 0: peer closed, penent already unregistered and closed the fd */
      }
    }
  }
}
```

## API reference

### Types

```c
typedef struct { 
    int32_t ident; 
    int16_t filter; 
    uint32_t flags; 
} penent_stats_t;
typedef /* struct epoll_event | struct kevent */ penent_event_t;
```

| Constant         | Meaning                                  |
| ---------------- | ---------------------------------------- |
| `PN_FILT_READ`   | Socket is readable                       |
| `PN_FILT_WRITE`  | Socket is writable                       |
| `PN_EOF`         | Peer closed the connection (`flags`)     |
| `PN_ERR`         | Error or hang-up on the descriptor (`flags`) |

### Functions

#### `int pn_init(void)`
Creates the event queue (`epoll_create1` / `kqueue`). Returns the queue fd, or `-1` on error.

#### `int pn_add_event(int pn_fd, int32_t ident, int16_t filter)`
Starts watching `ident` for `PN_FILT_READ` or `PN_FILT_WRITE`. Returns `0` on success (kqueue returns the `kevent` result), `-1` on error.

#### `int pn_del_event(int pn_fd, int32_t ident, int16_t filter)`
Stops watching `ident`. Returns `0` on success, `-1` on error.

#### `int pn_monitor(int pn_fd, penent_event_t *event_list, int max_events, int timeout_ms)`
Waits for events. `timeout_ms < 0` blocks indefinitely, `0` returns immediately. Returns the number of events stored in `event_list`, or `-1` on error.

#### `penent_stats_t pn_get_event(penent_event_t ev)`
Converts a platform-specific event into a portable `penent_stats_t` (`ident` = file descriptor, `filter` = read/write, `flags` = `PN_EOF` / `PN_ERR` when applicable).

#### `int pn_accept(int pn_fd, int fd)`
Accepts a connection on listening socket `fd`, makes the new socket non-blocking and registers it for read events. Returns the client fd, or `-1` on failure (the client socket is closed if setup fails).

#### `int pn_read(int fd, void *buff, size_t size, int pn_fd)`
Receives up to `size - 1` bytes (one byte is left free for you to add a `'\0'` if you need a string). Returns:

| Value | Meaning |
| ----- | ------- |
| `> 0` | Number of bytes read |
| `0`   | Peer closed the connection; the fd is unregistered and closed for you |
| `-1`  | Error |
| `-2`  | Would block (`EAGAIN` / `EWOULDBLOCK`) |

#### `int pn_write(int fd, void *buff, size_t size, int pn_fd)`
Sends `size` bytes. If everything was sent, the write watch on `fd` is removed. Returns:

| Value | Meaning |
| ----- | ------- |
| `>= 0` | Number of bytes sent (may be less than `size`) |
| `-1`  | Error |
| `-2`  | Would block (`EAGAIN` / `EWOULDBLOCK`) |

On `-2` or a short write, keep the remaining data and call `pn_add_event(pn_fd, fd, PN_FILT_WRITE)` to be notified when the socket is writable again.

## Platform notes

- **Backend selection** is automatic: macOS and the BSDs use kqueue; everything else uses epoll.
- **epoll registers one interest set per fd.** `pn_add_event` on an fd that is already registered replaces its filter (`EPOLL_CTL_MOD`), and `pn_del_event` removes the fd entirely, regardless of the filter you pass. kqueue keeps read and write filters independent.
- **Closing sockets:** on Linux the internal close helper calls `shutdown(fd, SHUT_RDWR)` before `close(fd)`.
- **Level-triggered** behavior is used on both backends.

## Limitations / TODO

- On epoll, `EPOLLRDHUP` is not requested in `pn_add_event`, so `PN_EOF` is not reported there; detect disconnects through `pn_read` returning `0`.
- `pn_write` uses `send(..., 0)`; consider `MSG_NOSIGNAL` (or ignoring `SIGPIPE`) so writing to a closed peer doesn't kill your process on Linux.
- IPv4 only for the client address in `pn_accept` is stored, but sockets of any family are accepted.
- Only TCP-style socket I/O is wrapped (no timers, signals or file watchers).

## Contributing

Issues and pull requests are welcome. Please keep the library small and dependency-free.

## License

Licensed under the [GNU General Public License v3.0](./LICENSE).
