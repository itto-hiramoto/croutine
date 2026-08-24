#ifndef CROUTINE_RUNTIME_H
#define CROUTINE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*croutine_task_fn)(void *arg);

typedef struct croutine_runtime_config {
    size_t worker_count; /* 0 selects the number of online CPUs. */
    size_t stack_size;   /* 0 selects the 256 KiB default. */
} croutine_runtime_config;

typedef enum croutine_io_wait_result {
    CROUTINE_IO_WAIT_FAILED = 0,
    CROUTINE_IO_WAIT_READY = 1,
    CROUTINE_IO_WAIT_CLOSED = 2,
} croutine_io_wait_result;

/*
 * A runtime instance is process-wide and one-shot. After shutdown, init will
 * return false. Pointer arguments remain owned by the caller. Copy arguments
 * are copied by the runtime and released when their task is destroyed.
 * Channels are caller-owned and may be destroyed after runtime shutdown.
 */
bool croutine_runtime_init(const croutine_runtime_config *config);
int croutine_runtime_run(croutine_task_fn main_fn, void *arg);
void croutine_runtime_request_shutdown(void);
void croutine_runtime_shutdown(void);
void croutine_runtime_set_exit_code(int code);

bool croutine_spawn(croutine_task_fn fn, void *arg);
bool croutine_spawn_copy(croutine_task_fn fn, const void *arg, size_t arg_size);
void croutine_yield(void);

croutine_io_wait_result croutine_io_wait_readable(int fd);
croutine_io_wait_result croutine_io_wait_writable(int fd);
void croutine_io_forget_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif
