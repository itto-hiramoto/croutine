#include <croutine/channel.h>
#include <croutine/io.h>
#include <croutine/runtime.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IO_TASKS 8

struct io_case {
    int read_fd;
    int write_fd;
};

static atomic_int failures;
static atomic_int copy_result;
static atomic_int io_completed;
static atomic_int runnable_after_parks;
static atomic_int forgotten_woke;
static atomic_int stress_steps;
static atomic_int dual_read_done;
static atomic_int dual_write_done;
static struct io_case dual_io;
static struct io_case shutdown_io;
static int reused_fd;
static atomic_int old_wait_done;
static atomic_int reused_wait_ready;
static croutine_channel *unbuffered_channel;
static croutine_channel *shutdown_channel;
static struct io_case io_cases[IO_TASKS];

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        atomic_fetch_add(&failures, 1);
    }
}

static void copied_argument_task(void *arg) {
    int value = *(int *)arg;
    atomic_store(&copy_result, value + 1);
}

static void stress_task(void *arg) {
    (void)arg;
    atomic_fetch_add(&stress_steps, 1);
    croutine_yield();
    atomic_fetch_add(&stress_steps, 1);
}

static void io_reader(void *arg) {
    struct io_case *io = arg;
    char byte = 0;
    ssize_t count = croutine_read(io->read_fd, &byte, 1);
    check(count == 1 && byte == 'x', "I/O reader value");
    atomic_fetch_add(&io_completed, 1);
}

static void channel_sender(void *arg) {
    (void)arg;
    int value = 73;
    check(croutine_channel_send(unbuffered_channel, &value) ==
              CROUTINE_CHANNEL_SEND_OK,
          "channel sender result");
}

static void forgotten_reader(void *arg) {
    struct io_case *io = arg;
    char byte;
    errno = 0;
    ssize_t result = croutine_read(io->read_fd, &byte, 1);
    check(result == -1 && errno == ECANCELED, "forgotten reader cancellation");
    atomic_store(&forgotten_woke, 1);
}

static void dual_reader(void *arg) {
    struct io_case *io = arg;
    char byte = 0;
    check(croutine_read(io->read_fd, &byte, 1) == 1 && byte == 'r',
          "simultaneous read waiter");
    atomic_store(&dual_read_done, 1);
}

static void dual_writer(void *arg) {
    struct io_case *io = arg;
    check(croutine_write(io->read_fd, "w", 1) == 1,
          "simultaneous write waiter");
    atomic_store(&dual_write_done, 1);
}

static void old_readiness_waiter(void *arg) {
    int fd = *(int *)arg;
    (void)croutine_io_wait_readable(fd);
    atomic_store(&old_wait_done, 1);
}

static void reused_readiness_waiter(void *arg) {
    int fd = *(int *)arg;
    if (croutine_io_wait_readable(fd) == CROUTINE_IO_WAIT_READY) {
        atomic_store(&reused_wait_ready, 1);
    }
}

static void parked_channel_receiver(void *arg) {
    (void)arg;
    int value;
    (void)croutine_channel_recv(shutdown_channel, &value);
}

static void parked_io_reader(void *arg) {
    struct io_case *io = arg;
    char byte;
    (void)croutine_read(io->read_fd, &byte, 1);
}

static void runtime_main(void *arg) {
    (void)arg;

    int copied = 41;
    check(croutine_spawn_copy(copied_argument_task, &copied, sizeof(copied)),
          "spawn copied argument");
    copied = -1;
    while (atomic_load(&copy_result) == 0) {
        croutine_yield();
    }
    check(atomic_load(&copy_result) == 42, "copied argument ownership");

    for (int i = 0; i < 2000; ++i) {
        check(croutine_spawn(stress_task, NULL), "spawn stress task");
    }
    while (atomic_load(&stress_steps) != 4000) {
        croutine_yield();
    }

    croutine_channel *buffered = croutine_channel_create(sizeof(int), 2);
    check(buffered != NULL, "create buffered channel");
    int sent = 9;
    int received = 0;
    check(croutine_channel_try_send(buffered, &sent), "buffered try send");
    check(croutine_channel_try_recv(buffered, &received), "buffered try recv");
    check(received == sent, "buffered channel value");
    check(croutine_channel_try_send(buffered, &sent), "select setup send");
    croutine_select_case select_case = {
        .channel = buffered,
        .op = CROUTINE_SELECT_OP_RECV,
        .value = &received,
    };
    check(croutine_select(&select_case, 1, false) == 0,
          "select chooses ready receive");
    check(select_case.status == CROUTINE_SELECT_STATUS_VALUE,
          "select receive status");
    croutine_channel_close(buffered);
    check(croutine_channel_destroy(buffered), "destroy buffered channel");

    unbuffered_channel = croutine_channel_create(sizeof(int), 0);
    check(unbuffered_channel != NULL, "create unbuffered channel");
    check(croutine_spawn(channel_sender, NULL), "spawn channel sender");
    croutine_yield();
    received = 0;
    check(croutine_channel_recv(unbuffered_channel, &received) ==
              CROUTINE_CHANNEL_RECV_VALUE,
          "unbuffered receive");
    check(received == 73, "unbuffered channel value");
    croutine_channel_close(unbuffered_channel);
    croutine_yield();
    check(croutine_channel_destroy(unbuffered_channel),
          "destroy unbuffered channel");
    unbuffered_channel = NULL;

    for (int i = 0; i < IO_TASKS; ++i) {
        int pair[2];
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
              "create socket pair");
        io_cases[i].read_fd = pair[0];
        io_cases[i].write_fd = pair[1];
        check(croutine_fd_set_nonblocking(pair[0]) == 0,
              "set reader nonblocking");
        check(croutine_fd_set_nonblocking(pair[1]) == 0,
              "set writer nonblocking");
        check(croutine_spawn(io_reader, &io_cases[i]), "spawn I/O reader");
    }

    int forgotten_pair[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, forgotten_pair) == 0,
          "create forgotten socket pair");
    struct io_case forgotten = {
        .read_fd = forgotten_pair[0],
        .write_fd = forgotten_pair[1],
    };
    check(croutine_fd_set_nonblocking(forgotten.read_fd) == 0,
          "set forgotten reader nonblocking");
    check(croutine_spawn(forgotten_reader, &forgotten),
          "spawn forgotten reader");
    croutine_yield();
    croutine_io_forget_fd(forgotten.read_fd);
    close(forgotten.read_fd);
    close(forgotten.write_fd);
    while (!atomic_load(&forgotten_woke)) {
        croutine_yield();
    }

    int old_pair[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, old_pair) == 0,
          "create fd-reuse old pair");
    reused_fd = old_pair[0];
    check(croutine_fd_set_nonblocking(reused_fd) == 0,
          "set fd-reuse old endpoint nonblocking");
    check(croutine_spawn(old_readiness_waiter, &reused_fd),
          "spawn old readiness waiter");
    croutine_yield();
    check(write(old_pair[1], "s", 1) == 1, "queue stale readiness");
    croutine_io_forget_fd(reused_fd);
    close(old_pair[0]);
    close(old_pair[1]);

    int new_pair[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, new_pair) == 0,
          "create fd-reuse new pair");
    if (new_pair[1] == reused_fd) {
        int endpoint = new_pair[0];
        new_pair[0] = new_pair[1];
        new_pair[1] = endpoint;
    }
    if (new_pair[0] != reused_fd) {
        check(dup2(new_pair[0], reused_fd) == reused_fd,
              "force descriptor-number reuse");
        close(new_pair[0]);
    }
    check(croutine_fd_set_nonblocking(reused_fd) == 0,
          "set reused endpoint nonblocking");
    check(croutine_spawn(reused_readiness_waiter, &reused_fd),
          "spawn reused readiness waiter");
    croutine_yield();
    check(!atomic_load(&reused_wait_ready), "ignore stale readiness after reuse");
    check(write(new_pair[1], "n", 1) == 1, "wake reused descriptor");
    while (!atomic_load(&reused_wait_ready) || !atomic_load(&old_wait_done)) {
        croutine_yield();
    }
    croutine_io_forget_fd(reused_fd);
    close(reused_fd);
    close(new_pair[1]);

    int dual_pair[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, dual_pair) == 0,
          "create dual-direction socket pair");
    dual_io.read_fd = dual_pair[0];
    dual_io.write_fd = dual_pair[1];
    check(croutine_fd_set_nonblocking(dual_pair[0]) == 0,
          "set dual endpoint nonblocking");
    check(croutine_fd_set_nonblocking(dual_pair[1]) == 0,
          "set dual peer nonblocking");
    char fill[4096];
    memset(fill, 'f', sizeof(fill));
    while (write(dual_pair[0], fill, sizeof(fill)) > 0) {
    }
    check(errno == EAGAIN || errno == EWOULDBLOCK, "fill socket send buffer");
    check(croutine_spawn(dual_reader, &dual_io), "spawn dual read waiter");
    check(croutine_spawn(dual_writer, &dual_io), "spawn dual write waiter");
    croutine_yield();
    while (read(dual_pair[1], fill, sizeof(fill)) > 0) {
    }
    check(errno == EAGAIN || errno == EWOULDBLOCK, "drain socket send buffer");
    check(write(dual_pair[1], "r", 1) == 1, "wake dual read waiter");
    while (!atomic_load(&dual_read_done) || !atomic_load(&dual_write_done)) {
        croutine_yield();
    }
    croutine_io_forget_fd(dual_pair[0]);
    croutine_io_forget_fd(dual_pair[1]);
    close(dual_pair[0]);
    close(dual_pair[1]);

    /* Every reader reaches epoll before this task is runnable again. */
    croutine_yield();
    atomic_store(&runnable_after_parks, 1);
    for (int i = 0; i < IO_TASKS; ++i) {
        check(write(io_cases[i].write_fd, "x", 1) == 1, "write wake byte");
    }
    while (atomic_load(&io_completed) != IO_TASKS) {
        croutine_yield();
    }
    check(atomic_load(&runnable_after_parks) == 1,
          "runnable work continued with parked I/O tasks");
    for (int i = 0; i < IO_TASKS; ++i) {
        croutine_io_forget_fd(io_cases[i].read_fd);
        croutine_io_forget_fd(io_cases[i].write_fd);
        close(io_cases[i].read_fd);
        close(io_cases[i].write_fd);
    }

    shutdown_channel = croutine_channel_create(sizeof(int), 0);
    check(shutdown_channel != NULL, "create shutdown channel");
    check(croutine_spawn(parked_channel_receiver, NULL),
          "spawn parked channel receiver");
    int shutdown_pair[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, shutdown_pair) == 0,
          "create shutdown socket pair");
    shutdown_io.read_fd = shutdown_pair[0];
    shutdown_io.write_fd = shutdown_pair[1];
    check(croutine_fd_set_nonblocking(shutdown_io.read_fd) == 0,
          "set shutdown reader nonblocking");
    check(croutine_spawn(parked_io_reader, &shutdown_io),
          "spawn parked I/O reader");
    croutine_yield();

    croutine_runtime_set_exit_code(atomic_load(&failures) ? 1 : 0);
}

int main(void) {
    croutine_runtime_config config = {
        .worker_count = 1,
        .stack_size = 64 * 1024,
    };
    if (!croutine_runtime_init(&config)) {
        fprintf(stderr, "FAIL: runtime init\n");
        return 1;
    }
    int result = croutine_runtime_run(runtime_main, NULL);
    croutine_runtime_shutdown();

    check(croutine_channel_is_closed(shutdown_channel),
          "shutdown closes channels with parked tasks");
    check(croutine_channel_destroy(shutdown_channel),
          "destroy shutdown channel");
    close(shutdown_io.read_fd);
    close(shutdown_io.write_fd);
    return result != 0 || atomic_load(&failures) != 0;
}
