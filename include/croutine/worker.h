#ifndef CROUTINE_WORKER_H
#define CROUTINE_WORKER_H

#include <stdbool.h>

bool worker_init(void);
void worker_deinit(void);
void *worker_loop(void *arg);
void spawn(void (*fn)(void));
void yield(void);

#endif // CROUTINE_WORKER_H
