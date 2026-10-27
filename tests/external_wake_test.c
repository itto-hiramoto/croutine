#include <croutine/channel.h>
#include <croutine/runtime.h>

#include <pthread.h>
#include <sched.h>
#include <time.h>

static croutine_channel *channel;

static void *external_sender(void *arg) {
    (void)arg;
    // Let the only worker park the receiver and block in the poller.
    nanosleep(&(struct timespec){.tv_nsec = 100 * 1000 * 1000}, NULL);
    int value = 42;
    while (!croutine_channel_try_send(channel, &value)) {
        sched_yield();
    }
    return NULL;
}

static void application(void *arg) {
    (void)arg;
    int value = 0;
    if (croutine_channel_recv(channel, &value) != CROUTINE_CHANNEL_RECV_VALUE ||
        value != 42) {
        croutine_runtime_set_exit_code(1);
    }
    croutine_channel_close(channel);
}

int main(void) {
    channel = croutine_channel_create(sizeof(int), 0);
    if (!channel) {
        return 1;
    }
    pthread_t sender;
    if (pthread_create(&sender, NULL, external_sender, NULL) != 0) {
        return 1;
    }
    croutine_runtime_config config = {
        .worker_count = 1,
        .stack_size = 0,
    };
    const int exit_code = croutine_run(&config, application, NULL);
    pthread_join(sender, NULL);
    return exit_code == 0 && croutine_channel_destroy(channel) ? 0 : 1;
}
