#include "penent.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static int set_non_blocking(int fd) {
    int flags;

    if ((flags = fcntl(fd, F_GETFL, 0)) < 0) {
        return -1;
    };

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#if __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__

#define CLOSE(fd) close((fd))

#include <sys/event.h>

static inline int16_t get_filter(int filter) {

    if (filter == PN_FILT_READ)
        return EVFILT_READ;

    else if (filter == PN_FILT_WRITE)
        return EVFILT_WRITE;

    return 0;
}

int pn_init() {
    return kqueue();
}

int pn_add_event(int pn_fd, int32_t ident, int16_t filter) {

    struct kevent kev;
    int16_t f;

    if (!(f = get_filter(filter)))
        return -1;

    EV_SET(&kev, ident, f, EV_ADD | EV_ENABLE, 0, 0, NULL);

    return kevent(pn_fd, &kev, 1, NULL, 0, NULL);
}

int pn_del_event(int pn_fd, int32_t ident, int16_t filter) {
    struct kevent kev;
    int16_t f;

    if (!(f = get_filter(filter)))
        return -1;

    EV_SET(&kev, ident, f, EV_DELETE, 0, 0, NULL);

    return kevent(pn_fd, &kev, 1, NULL, 0, NULL);
}

int pn_monitor(int pn_fd, penent_event_t *event_list, int max_event,
               int timeout_ms) {
    struct timespec ts;
    struct timespec *pts = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        pts = &ts;
    }
    return kevent(pn_fd, NULL, 0, event_list, max_event, pts);
}

penent_stats_t pn_get_event(penent_event_t ev) {
    penent_stats_t pn = {0};

    pn.ident = (int)ev.ident;

    if (ev.flags & EV_ERROR)
        pn.flags = PN_ERR;

    if (ev.flags & EV_EOF)
        pn.flags = PN_EOF;

    if (ev.filter == EVFILT_READ)
        pn.filter = PN_FILT_READ;
    else if (ev.filter == EVFILT_WRITE)
        pn.filter = PN_FILT_WRITE;
    else
        pn.filter = PN_UNKNOWN;

    return pn;
}

#else

#include <sys/epoll.h>

#define CLOSE(fd)                                                              \
    do {                                                                       \
        shutdown((fd), SHUT_RDWR);                                             \
        close((fd));                                                           \
    } while (0)

int pn_init() {
    return epoll_create1(0);
}

static inline uint32_t get_filter(int filter) {

    if (filter == PN_FILT_READ)
        return EPOLLIN;

    else if (filter == PN_FILT_WRITE)
        return EPOLLOUT;

    return 0;
}

int pn_add_event(int pn_fd, int32_t ident, int16_t filter) {
    struct epoll_event ev;
    ev.events = get_filter(filter);
    ev.data.fd = ident;

    if (epoll_ctl(pn_fd, EPOLL_CTL_ADD, ident, &ev) < 0) {
        if (errno == EEXIST) {
            return epoll_ctl(pn_fd, EPOLL_CTL_MOD, ident, &ev);
        }

        return -1;
    }

    return 0;
}

int pn_del_event(int pn_fd, int32_t ident, int16_t filter) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ident;

    return epoll_ctl(pn_fd, EPOLL_CTL_DEL, ident, &ev);
}

int pn_monitor(int pn_fd, penent_event_t *event_list, int max_events,
               int timeout_ms) {
    return epoll_wait(pn_fd, event_list, max_events, timeout_ms);
}

penent_stats_t pn_get_event(penent_event_t ev) {
    penent_stats_t pn = {0};

    pn.ident = (int)ev.data.fd;

    if (ev.events & (EPOLLERR | EPOLLHUP))
        pn.flags = PN_ERR;

    if (ev.events & EPOLLRDHUP)
        pn.flags = PN_EOF;

    if (ev.events & EPOLLIN)
        pn.filter = PN_FILT_READ;
    else if (ev.events & EPOLLOUT)
        pn.filter = PN_FILT_WRITE;
    else
        pn.filter = PN_UNKNOWN

            return pn;
}

#endif

int pn_accept(int pn_fd, int fd) {
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int clientfd;

    if ((clientfd = accept(fd, (struct sockaddr *)&client_addr, &len)) < 0) {
        return -1;
    }

    if (set_non_blocking(clientfd) < 0) {
        CLOSE(clientfd);
        return -1;
    }

    if (pn_add_event(pn_fd, clientfd, PN_FILT_READ) < 0) {
        CLOSE(clientfd);
        return -1;
    }

    return clientfd;
}

int pn_write(int fd, void *buff, size_t size, int pn_fd) {

    ssize_t n = send(fd, buff, size, 0);

    if (n == size)
        pn_del_event(pn_fd, fd, PN_FILT_WRITE);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -2;

        return -1;
    }

    return n;
}

int pn_read(int fd, void *buff, size_t size, int pn_fd) {

    ssize_t n = recv(fd, buff, size - 1, 0);

    if (n == 0) {
        pn_del_event(pn_fd, fd, PN_FILT_READ);
        CLOSE(fd);
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -2;

        return -1;
    }

    return n; // n == 0 means client closed connection
}
