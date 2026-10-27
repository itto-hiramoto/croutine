#include <croutine/runtime.h>

static void application(void *arg) {
    (void)arg;
    croutine_runtime_set_exit_code(17);
}

int main(void) {
    croutine_runtime_config invalid = {
        .worker_count = 1,
        .stack_size = 4096,
    };
    if (croutine_runtime_init(&invalid)) {
        return 1;
    }

    croutine_runtime_config valid = {
        .worker_count = 1,
        .stack_size = 64 * 1024,
    };
    if (!croutine_runtime_init(&valid)) {
        return 1;
    }
    if (croutine_runtime_run(application, NULL) != 17) {
        return 1;
    }
    if (croutine_runtime_run(application, NULL) != -1) {
        return 1;
    }
    croutine_runtime_shutdown();
    return croutine_runtime_init(&valid) ? 1 : 0;
}
