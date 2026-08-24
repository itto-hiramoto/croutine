#include <croutine/runtime.h>

#include <stdio.h>

static void child(void *arg) { printf("child: %s\n", (const char *)arg); }

static void application(void *arg) {
    (void)arg;
    (void)croutine_spawn(child, "hello");
    croutine_yield();
}

int main(void) {
    croutine_runtime_config config = {0};
    config.worker_count = 2;
    if (!croutine_runtime_init(&config)) {
        return 1;
    }
    int result = croutine_runtime_run(application, NULL);
    croutine_runtime_shutdown();
    return result;
}
