#include <croutine/runtime.h>

#include <fenv.h>
#include <stdatomic.h>

#define YIELD_COUNT 100

static atomic_int finished_tasks;

static void rounding_task(void *arg) {
    (void)arg;
    fesetround(FE_UPWARD);
    for (int i = 0; i < YIELD_COUNT; ++i) {
        croutine_yield();
        if (fegetround() != FE_UPWARD) {
            croutine_runtime_set_exit_code(1);
        }
    }
    atomic_fetch_add(&finished_tasks, 1);
}

static void default_task(void *arg) {
    (void)arg;
    for (int i = 0; i < YIELD_COUNT; ++i) {
        if (fegetround() != FE_TONEAREST) {
            croutine_runtime_set_exit_code(1);
        }
        croutine_yield();
    }
    atomic_fetch_add(&finished_tasks, 1);
}

static void application(void *arg) {
    (void)arg;
    if (!croutine_spawn(rounding_task, NULL) ||
        !croutine_spawn(default_task, NULL)) {
        croutine_runtime_set_exit_code(1);
        return;
    }
    while (atomic_load(&finished_tasks) != 2) {
        croutine_yield();
    }
}

int main(void) {
    croutine_runtime_config config = {
        .worker_count = 2,
        .stack_size = 0,
    };
    return croutine_run(&config, application, NULL);
}
