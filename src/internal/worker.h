#ifndef CROUTINE_WORKER_H
#define CROUTINE_WORKER_H

#include <croutine/runtime.h>
#include "internal/task.h"
#include <stdbool.h>
#include <stdint.h>

// The task currently executing on this OS thread, or NULL when the worker
// is in scheduler/system code.
//
// Why _Thread_local:
//   1. Each worker pthread runs at most one green-thread task at a time, so
//      "current task" is intrinsically per-OS-thread state — no shared
//      writes, no locking needed.
// This is the single source of truth for the runtime's "current task".
// Do not introduce a parallel per-worker mirror.
extern _Thread_local struct Task *croutine_current_task;

bool worker_init(void);
void worker_configure_stack_size(size_t stack_size);
void worker_deinit(void);
void worker_request_shutdown(void);
void *worker_loop(void *arg);
bool worker_spawn(croutine_task_fn fn, const void *arg, size_t arg_size,
                  bool copy_arg, bool is_main);
croutine_io_wait_result worker_park_current_on_io(int fd, uint32_t events);
void worker_forget_fd(int fd);
// Print `message` to stderr and abort. For scheduler and channel states that
// cannot be recovered from, where continuing would corrupt shared structures.
_Noreturn void worker_fail(const char *message);
void worker_scheduler_lock(void);
void worker_scheduler_unlock(void);
bool worker_shutdown_requested_locked(void);
struct Task *worker_current_task(void);
// Park the current task until something wakes it. Call with the scheduler lock
// held. Unlike every other `_locked` function here, this always returns without
// the lock: on a successful park it is handed to the scheduler across the
// context switch, and otherwise it is released before returning.
//
// The return value is the wake outcome, not whether parking happened. It is
// false both when the task could not park at all (no current task, or shutdown
// already requested) and when it parked and was then woken with
// `success = false`, which is what `croutine_channel_close` does to regular
// waiters. Callers that need to tell those apart must inspect their own
// waiter.
bool worker_park_current_on_channel_locked(void);
bool worker_wake_task_locked(struct Task *task, bool success);
void croutine_channel_shutdown_all_locked(void);
void worker_yield(void);
void worker_reset_main_state(void);
int worker_wait_for_main(void);

#endif // CROUTINE_WORKER_H
