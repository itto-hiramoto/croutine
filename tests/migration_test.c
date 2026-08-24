#include <croutine/runtime.h>

#include <stdatomic.h>

#define TASK_COUNT 5000

static atomic_int steps;

static void migrating_task(void *arg) {
    (void)arg;
    atomic_fetch_add(&steps, 1);
    croutine_yield();
    atomic_fetch_add(&steps, 1);
    croutine_yield();
    atomic_fetch_add(&steps, 1);
}

static void application(void *arg) {
    (void)arg;
    for (int i = 0; i < TASK_COUNT; ++i) {
        if (!croutine_spawn(migrating_task, NULL)) {
            croutine_runtime_set_exit_code(1);
            return;
        }
    }
    while (atomic_load(&steps) != TASK_COUNT * 3) {
        croutine_yield();
    }
}

int main(void) {
    croutine_runtime_config config = {
        .worker_count = 4,
        .stack_size = 64 * 1024,
    };
    if (!croutine_runtime_init(&config)) {
        return 1;
    }
    int result = croutine_runtime_run(application, NULL);
    croutine_runtime_shutdown();
    return result;
}
