#include <croutine/worker.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void begin_runtime(void) {
    const int num_threads = sysconf(_SC_NPROCESSORS_ONLN);

    if (num_threads == -1) {
        fprintf(stderr, "Failed to get number of threads: %s\n",
                strerror(errno));
        return;
    }

    if (!worker_init()) {
        fprintf(stderr, "Failed to initialize worker\n");
        return;
    }

    pthread_t threads[num_threads];
    for (int worker_id = 0; worker_id < num_threads; ++worker_id) {
        pthread_create(&threads[worker_id], NULL, worker_loop,
                       (void *)(intptr_t)worker_id);
    }

    for (int worker_id = 0; worker_id < num_threads; ++worker_id) {
        pthread_join(threads[worker_id], NULL);
    }

    worker_deinit();
}
