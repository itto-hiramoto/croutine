#include <croutine/runtime.h>

#include <stddef.h>

static void application(void *arg) {
    (void)arg;
    croutine_runtime_set_exit_code(23);
}

int main(void) {
    croutine_runtime_config invalid = {
        .worker_count = 1,
        .stack_size = 4096,
    };
    if (croutine_run(&invalid, application, NULL) != -1) {
        return 1;
    }

    croutine_runtime_config valid = {
        .worker_count = 2,
        .stack_size = 64 * 1024,
    };
    return croutine_run(&valid, application, NULL) == 23 ? 0 : 1;
}
