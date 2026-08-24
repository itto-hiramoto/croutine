#include "internal/poller.h"
#include "internal/task.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int poller_fd = -1;
static int wake_fd = -1;
static uint32_t *fd_generations;
static unsigned char *fd_registered;
static size_t fd_table_capacity;
static pthread_mutex_t fd_table_mutex = PTHREAD_MUTEX_INITIALIZER;

#define WAKE_TOKEN UINT64_MAX

static bool ensure_fd_capacity(int fd) {
    if (fd < 0) {
        errno = EINVAL;
        return false;
    }
    size_t needed = (size_t)fd + 1;
    if (needed <= fd_table_capacity) {
        return true;
    }
    size_t capacity = fd_table_capacity ? fd_table_capacity : 64;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            errno = ENOMEM;
            return false;
        }
        capacity *= 2;
    }
    uint32_t *generations = calloc(capacity, sizeof(*generations));
    unsigned char *registered = calloc(capacity, sizeof(*registered));
    if (!generations || !registered) {
        free(generations);
        free(registered);
        errno = ENOMEM;
        return false;
    }
    if (fd_table_capacity) {
        memcpy(generations, fd_generations,
               fd_table_capacity * sizeof(*generations));
        memcpy(registered, fd_registered,
               fd_table_capacity * sizeof(*registered));
    }
    free(fd_generations);
    free(fd_registered);
    fd_generations = generations;
    fd_registered = registered;
    fd_table_capacity = capacity;
    return true;
}

static uint64_t fd_token(int fd) {
    return ((uint64_t)fd_generations[fd] << 32) | (uint32_t)fd;
}

static uint32_t to_epoll_events(uint32_t events) {
    uint32_t epoll_events = 0;
    if ((events & CROUTINE_IO_EVENT_READ) != 0) {
        epoll_events |= EPOLLIN | EPOLLRDHUP | EPOLLPRI;
    }
    if ((events & CROUTINE_IO_EVENT_WRITE) != 0) {
        epoll_events |= EPOLLOUT;
    }
    return epoll_events;
}

static uint32_t from_epoll_events(uint32_t epoll_events) {
    uint32_t events = CROUTINE_IO_EVENT_NONE;
    if ((epoll_events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0) {
        events |= CROUTINE_IO_EVENT_READ;
    }
    if ((epoll_events & EPOLLOUT) != 0) {
        events |= CROUTINE_IO_EVENT_WRITE;
    }
    if ((epoll_events & (EPOLLERR | EPOLLHUP)) != 0) {
        events |= CROUTINE_IO_EVENT_READ | CROUTINE_IO_EVENT_WRITE;
    }
    return events;
}

static bool poller_ctl(int op, int fd, uint32_t events, uint64_t token) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = to_epoll_events(events);
    event.data.u64 = token;

    if (epoll_ctl(poller_fd, op, fd, &event) == 0) {
        return true;
    }

    fprintf(stderr, "epoll_ctl failed: %s\n", strerror(errno));
    return false;
}

static void drain_wake_fd(void) {
    eventfd_t value;
    // eventfd becomes readable while its internal counter is non-zero.
    // Reading drains the pending wakeup count back to zero, so epoll will not
    // keep reporting wake_fd as readable forever.
    if (eventfd_read(wake_fd, &value) != 0 && errno != EAGAIN) {
        fprintf(stderr, "Failed to drain poller wake fd: %s\n",
                strerror(errno));
    }
}

bool croutine_poller_init(void) {
    if (poller_fd >= 0) {
        return true;
    }

    poller_fd = epoll_create1(EPOLL_CLOEXEC);
    if (poller_fd < 0) {
        fprintf(stderr, "Failed to create epoll instance: %s\n",
                strerror(errno));
        return false;
    }

    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd < 0) {
        fprintf(stderr, "Failed to create eventfd: %s\n", strerror(errno));
        close(poller_fd);
        poller_fd = -1;
        return false;
    }

    // Register the internal wake fd as a read event. croutine_poller_wake()
    // writes to the eventfd, and drain_wake_fd() consumes that counter so the
    // poller can go back to sleep after handling the wakeup.
    struct epoll_event wake_event;
    memset(&wake_event, 0, sizeof(wake_event));
    wake_event.events = EPOLLIN;
    wake_event.data.u64 = WAKE_TOKEN;
    if (epoll_ctl(poller_fd, EPOLL_CTL_ADD, wake_fd, &wake_event) != 0) {
        close(wake_fd);
        close(poller_fd);
        wake_fd = -1;
        poller_fd = -1;
        return false;
    }

    return true;
}

void croutine_poller_deinit(void) {
    if (wake_fd >= 0) {
        close(wake_fd);
        wake_fd = -1;
    }
    if (poller_fd >= 0) {
        close(poller_fd);
        poller_fd = -1;
    }
    free(fd_generations);
    free(fd_registered);
    fd_generations = NULL;
    fd_registered = NULL;
    fd_table_capacity = 0;
}

static bool poller_set_locked(int fd, uint32_t old_events,
                              uint32_t new_events) {
    if (old_events == new_events) {
        return true;
    }

    if (new_events == CROUTINE_IO_EVENT_NONE) {
        if (epoll_ctl(poller_fd, EPOLL_CTL_DEL, fd, NULL) == 0 ||
            errno == ENOENT) {
            if ((size_t)fd < fd_table_capacity) {
                fd_registered[fd] = 0;
                fd_generations[fd]++;
            }
            return true;
        }

        fprintf(stderr, "epoll_ctl delete failed: %s\n", strerror(errno));
        return false;
    }

    if (old_events == CROUTINE_IO_EVENT_NONE) {
        if (!ensure_fd_capacity(fd)) {
            return false;
        }
        fd_generations[fd]++;
        if (!poller_ctl(EPOLL_CTL_ADD, fd, new_events, fd_token(fd))) {
            return false;
        }
        fd_registered[fd] = 1;
        return true;
    }

    if ((size_t)fd >= fd_table_capacity || !fd_registered[fd]) {
        errno = ENOENT;
        return false;
    }
    return poller_ctl(EPOLL_CTL_MOD, fd, new_events, fd_token(fd));
}

bool croutine_poller_set(int fd, uint32_t old_events, uint32_t new_events) {
    pthread_mutex_lock(&fd_table_mutex);
    bool result = poller_set_locked(fd, old_events, new_events);
    pthread_mutex_unlock(&fd_table_mutex);
    return result;
}

int croutine_poller_wait(struct CroutinePollEvent *events, int max_events,
                         int timeout_ms) {
    if (!events || max_events <= 0) {
        errno = EINVAL;
        return -1;
    }

    struct epoll_event ready_events[max_events];
    int ready;
    for (;;) {
        ready = epoll_wait(poller_fd, ready_events, max_events, timeout_ms);
        if (ready >= 0) {
            break;
        }
        // Signal delivery can interrupt epoll_wait; retry transparently.
        if (errno != EINTR) {
            return -1;
        }
    }

    int count = 0;
    pthread_mutex_lock(&fd_table_mutex);
    for (int i = 0; i < ready; ++i) {
        uint64_t token = ready_events[i].data.u64;
        if (token == WAKE_TOKEN) {
            drain_wake_fd();
            continue;
        }

        int fd = (int)(uint32_t)token;
        uint32_t generation = (uint32_t)(token >> 32);
        if ((size_t)fd >= fd_table_capacity || !fd_registered[fd] ||
            fd_generations[fd] != generation) {
            continue;
        }

        const uint32_t translated = from_epoll_events(ready_events[i].events);
        if (translated == CROUTINE_IO_EVENT_NONE) {
            continue;
        }

        events[count].fd = fd;
        events[count].events = translated;
        count++;
    }
    pthread_mutex_unlock(&fd_table_mutex);

    return count;
}

bool croutine_poller_wake(void) {
    eventfd_t value = 1;
    if (eventfd_write(wake_fd, value) == 0) {
        return true;
    }

    if (errno == EAGAIN) {
        return true;
    }

    fprintf(stderr, "Failed to wake poller: %s\n", strerror(errno));
    return false;
}
