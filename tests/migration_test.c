#include <croutine/channel.h>
#include <croutine/runtime.h>

#include <stdatomic.h>
#include <stdint.h>

#define TASK_COUNT 5000
#define PAIR_COUNT 8
#define ROUND_COUNT 2000

static atomic_int steps;
static atomic_int finished_ping_pong_tasks;
static croutine_channel *pings[PAIR_COUNT];
static croutine_channel *pongs[PAIR_COUNT];

static void migrating_task(void *arg) {
    (void)arg;
    atomic_fetch_add(&steps, 1);
    croutine_yield();
    atomic_fetch_add(&steps, 1);
    croutine_yield();
    atomic_fetch_add(&steps, 1);
}

static void pinger(void *arg) {
    const size_t pair = (size_t)(uintptr_t)arg;
    for (int round = 0; round < ROUND_COUNT; ++round) {
        int value = round;
        if (croutine_channel_send(pings[pair], &value) !=
                CROUTINE_CHANNEL_SEND_OK ||
            croutine_channel_recv(pongs[pair], &value) !=
                CROUTINE_CHANNEL_RECV_VALUE ||
            value != round) {
            croutine_runtime_set_exit_code(1);
        }
    }
    atomic_fetch_add(&finished_ping_pong_tasks, 1);
}

static void ponger(void *arg) {
    const size_t pair = (size_t)(uintptr_t)arg;
    for (int round = 0; round < ROUND_COUNT; ++round) {
        int value;
        if (croutine_channel_recv(pings[pair], &value) !=
                CROUTINE_CHANNEL_RECV_VALUE ||
            croutine_channel_send(pongs[pair], &value) !=
                CROUTINE_CHANNEL_SEND_OK) {
            croutine_runtime_set_exit_code(1);
        }
    }
    atomic_fetch_add(&finished_ping_pong_tasks, 1);
}

static bool run_ping_pong(void) {
    for (size_t pair = 0; pair < PAIR_COUNT; ++pair) {
        pings[pair] = croutine_channel_create(sizeof(int), 0);
        pongs[pair] = croutine_channel_create(sizeof(int), 0);
        if (!pings[pair] || !pongs[pair] ||
            !croutine_spawn(pinger, (void *)(uintptr_t)pair) ||
            !croutine_spawn(ponger, (void *)(uintptr_t)pair)) {
            return false;
        }
    }
    while (atomic_load(&finished_ping_pong_tasks) != PAIR_COUNT * 2) {
        croutine_yield();
    }
    for (size_t pair = 0; pair < PAIR_COUNT; ++pair) {
        croutine_channel_close(pings[pair]);
        croutine_channel_close(pongs[pair]);
        if (!croutine_channel_destroy(pings[pair]) ||
            !croutine_channel_destroy(pongs[pair])) {
            return false;
        }
    }
    return true;
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
    if (!run_ping_pong()) {
        croutine_runtime_set_exit_code(1);
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
