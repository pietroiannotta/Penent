#ifndef _PENENT_H_
#define _PENENT_H_

#include <stdint.h>
#include <stdio.h>

#if __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__

#include <sys/event.h>
typedef struct kevent penent_event_t;

#else

#include <sys/epoll.h>
typedef struct epoll_event penent_event_t;

#endif

enum {
  PN_FILT_READ,
  PN_FILT_WRITE,
  PN_EOF,
  PN_ERR,
  PN_UNKNOWN,
};

typedef struct {
  int32_t ident;
  int16_t filter;
  uint32_t flags;
} penent_stats_t;

int pn_init();
int pn_monitor(int pn_fd, penent_event_t *event_list, int max_event,
               int timeout_ms);

int pn_write(int fd, void *buff, size_t size, int pn_fd);
int pn_read(int fd, void *buff, size_t size, int pn_fd);
int pn_accept(int pn_fd, int fd);

penent_stats_t pn_get_event(penent_event_t ev);
int pn_add_event(int pn_fd, int32_t ident, int16_t filter);
int pn_del_event(int pn_fd, int32_t ident, int16_t filter);

#endif
