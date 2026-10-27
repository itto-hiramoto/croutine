#include "internal/poller.h"

#include <errno.h>

bool croutine_poller_init(void) {
    errno = ENOSYS;
    return false;
}

void croutine_poller_deinit(void) {}

bool croutine_poller_set(int fd, uint32_t old_events, uint32_t new_events) {
    (void)fd;
    (void)old_events;
    (void)new_events;
    errno = ENOSYS;
    return false;
}

int croutine_poller_wait(struct CroutinePollEvent *events, int max_events,
                         int timeout_ms) {
    (void)events;
    (void)max_events;
    (void)timeout_ms;
    errno = ENOSYS;
    return -1;
}

bool croutine_poller_wake(void) {
    errno = ENOSYS;
    return false;
}
