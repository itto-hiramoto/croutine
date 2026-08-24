#include "internal/task.h"
#include "internal/worker.h"
#include <croutine/runtime.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t runtime_worker_count;
static size_t runtime_started_workers;
static pthread_t *runtime_workers;
static bool runtime_initialized;
static bool runtime_ever_initialized;
static bool runtime_ran;

bool croutine_runtime_init(const croutine_runtime_config *config) {
    if (runtime_initialized || runtime_ever_initialized) {
        return false;
    }
    size_t worker_count = config ? config->worker_count : 0;
    if (worker_count == 0) {
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        if (online <= 0) {
            return false;
        }
        worker_count = (size_t)online;
    }

    size_t stack_size = config ? config->stack_size : 0;
    if (stack_size == 0) {
        stack_size = CROUTINE_DEFAULT_STACK_SIZE;
    }
    if (stack_size < 16u * 1024u) {
        return false;
    }

    worker_configure_stack_size(stack_size);
    worker_reset_main_state();
    if (!worker_init()) {
        runtime_ever_initialized = true;
        return false;
    }

    runtime_ever_initialized = true;
    runtime_worker_count = worker_count;
    runtime_initialized = true;
    return true;
}

int croutine_runtime_run(croutine_task_fn main_fn, void *arg) {
    if (!runtime_initialized || runtime_ran || !main_fn) {
        return -1;
    }
    runtime_ran = true;

    if (!worker_spawn(main_fn, arg, 0, false, true)) {
        return -1;
    }

    runtime_workers = calloc(runtime_worker_count, sizeof(*runtime_workers));
    if (!runtime_workers) {
        worker_request_shutdown();
        return -1;
    }

    for (size_t worker_id = 0; worker_id < runtime_worker_count; ++worker_id) {
        int error = pthread_create(&runtime_workers[worker_id], NULL,
                                   worker_loop, (void *)(uintptr_t)worker_id);
        if (error != 0) {
            fprintf(stderr, "Failed to create worker thread: %s\n",
                    strerror(error));
            worker_request_shutdown();
            break;
        }
        runtime_started_workers++;
    }

    int exit_code = runtime_started_workers == runtime_worker_count
                        ? worker_wait_for_main()
                        : -1;

    for (size_t i = 0; i < runtime_started_workers; ++i) {
        (void)pthread_join(runtime_workers[i], NULL);
    }
    free(runtime_workers);
    runtime_workers = NULL;
    runtime_started_workers = 0;
    return exit_code;
}

void croutine_runtime_request_shutdown(void) {
    if (runtime_initialized) {
        worker_request_shutdown();
    }
}

void croutine_runtime_shutdown(void) {
    if (!runtime_initialized) {
        return;
    }
    worker_request_shutdown();
    for (size_t i = 0; i < runtime_started_workers; ++i) {
        (void)pthread_join(runtime_workers[i], NULL);
    }
    free(runtime_workers);
    runtime_workers = NULL;
    runtime_started_workers = 0;
    worker_deinit();
    runtime_initialized = false;
}

croutine_io_wait_result croutine_io_wait_readable(int fd) {
    return worker_park_current_on_io(fd, CROUTINE_IO_EVENT_READ);
}

croutine_io_wait_result croutine_io_wait_writable(int fd) {
    return worker_park_current_on_io(fd, CROUTINE_IO_EVENT_WRITE);
}

void croutine_io_forget_fd(int fd) { worker_forget_fd(fd); }
