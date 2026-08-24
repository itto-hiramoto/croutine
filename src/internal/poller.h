#ifndef CROUTINE_POLLER_H
#define CROUTINE_POLLER_H

#include <stdbool.h>
#include <stdint.h>

struct CroutinePollEvent {
    int fd;
    uint32_t events;
};

bool croutine_poller_init(void);
void croutine_poller_deinit(void);
bool croutine_poller_set(int fd, uint32_t old_events, uint32_t new_events);
int croutine_poller_wait(struct CroutinePollEvent *events, int max_events, int timeout_ms);
bool croutine_poller_wake(void);

#endif // CROUTINE_POLLER_H


