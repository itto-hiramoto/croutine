#include "internal/worker.h"
#include "internal/poller.h"
#include "internal/task.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_RUN_QUEUE_CAPACITY 1024
#define INITIAL_IO_WAIT_CAPACITY 64
#define MAX_POLLER_EVENTS 64

static struct TaskQueue runnable_tasks;
static pthread_mutex_t scheduler_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scheduler_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t runtime_init_once = PTHREAD_ONCE_INIT;
static bool runtime_init_ok = false;
static bool shutdown_requested = false;
static bool poller_waiter_active = false;
static pthread_mutex_t main_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t main_cond = PTHREAD_COND_INITIALIZER;
static bool main_finished = false;
static bool scheduler_main_finished = false;
static bool main_spawned = false;
static int main_exit_code = 0;
static size_t configured_stack_size = CROUTINE_DEFAULT_STACK_SIZE;

// Per-OS-thread worker state. The currently-executing task is tracked by
// the file-scope `croutine_current_task` below. Do not introduce a parallel
// per-worker mirror.
struct Worker {
    struct Context context;
    void *asan_fake_stack;
    // The state the current task yielded back to the scheduler with.
    enum TaskState yielded_state;
    bool returned_with_scheduler_lock;
};

struct IoWaitEntry {
    int fd;
    struct Task *read_task;
    struct Task *write_task;
    bool in_use;
};

struct IoWaitTable {
    struct IoWaitEntry *entries;
    size_t capacity;
    size_t count;
};

static struct IoWaitTable io_waits;

_Thread_local struct Worker worker;
_Thread_local struct Task *croutine_current_task = NULL;

#if defined(CROUTINE_USE_ASAN)
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom,
                                    size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                     const void **bottom_old, size_t *size_old);

static void switch_from_scheduler(struct Task *task) {
    __sanitizer_start_switch_fiber(&worker.asan_fake_stack, task->stack,
                                   task->stack_size);
    context_switch(&worker.context, &task->context);
    __sanitizer_finish_switch_fiber(worker.asan_fake_stack, NULL, NULL);
}

static void start_switch_from_task(struct Task *task) {
    __sanitizer_start_switch_fiber(&task->asan_fake_stack,
                                   task->asan_scheduler_stack,
                                   task->asan_scheduler_stack_size);
}

static void finish_switch_to_task(struct Task *task) {
    __sanitizer_finish_switch_fiber(task->asan_fake_stack,
                                    &task->asan_scheduler_stack,
                                    &task->asan_scheduler_stack_size);
}
#else
static void switch_from_scheduler(struct Task *task) {
    context_switch(&worker.context, &task->context);
}
static void start_switch_from_task(struct Task *task) { (void)task; }
static void finish_switch_to_task(struct Task *task) { (void)task; }
#endif

_Noreturn void worker_fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}

static void cleanup_task(struct Task *task) {
    if (!task) {
        return;
    }

    task->scheduler.state = TASK_FINISHED;
    task_cleanup(task);
    free(task);
}

static bool update_poller_interest_locked(int fd, uint32_t old_events,
                                          uint32_t new_events);

static uint32_t io_wait_entry_events(const struct IoWaitEntry *entry) {
    uint32_t events = CROUTINE_IO_EVENT_NONE;
    if (entry->read_task) {
        events |= CROUTINE_IO_EVENT_READ;
    }
    if (entry->write_task) {
        events |= CROUTINE_IO_EVENT_WRITE;
    }
    return events;
}

static void io_wait_entry_reset(struct IoWaitEntry *entry) {
    entry->fd = -1;
    entry->read_task = NULL;
    entry->write_task = NULL;
    entry->in_use = false;
}

static bool io_wait_table_init(struct IoWaitTable *table, size_t capacity) {
    table->entries = calloc(capacity, sizeof(struct IoWaitEntry));
    if (!table->entries) {
        table->capacity = 0;
        table->count = 0;
        return false;
    }

    table->capacity = capacity;
    table->count = 0;
    for (size_t i = 0; i < capacity; ++i) {
        io_wait_entry_reset(&table->entries[i]);
    }
    return true;
}

static void io_wait_table_deinit(struct IoWaitTable *table) {
    if (!table) {
        return;
    }

    free(table->entries);
    table->entries = NULL;
    table->capacity = 0;
    table->count = 0;
}

static bool io_wait_table_grow(struct IoWaitTable *table) {
    const size_t new_capacity = table->capacity ? table->capacity * 2 : 1;
    struct IoWaitEntry *entries =
        calloc(new_capacity, sizeof(struct IoWaitEntry));
    if (!entries) {
        return false;
    }

    for (size_t i = 0; i < new_capacity; ++i) {
        io_wait_entry_reset(&entries[i]);
    }

    size_t dst = 0;
    for (size_t i = 0; i < table->capacity; ++i) {
        if (!table->entries[i].in_use) {
            continue;
        }
        entries[dst++] = table->entries[i];
    }

    free(table->entries);
    table->entries = entries;
    table->capacity = new_capacity;
    table->count = dst;
    return true;
}

static struct IoWaitEntry *io_wait_table_find(struct IoWaitTable *table,
                                              int fd) {
    for (size_t i = 0; i < table->capacity; ++i) {
        if (table->entries[i].in_use && table->entries[i].fd == fd) {
            return &table->entries[i];
        }
    }
    return NULL;
}

static struct IoWaitEntry *
io_wait_table_get_or_insert(struct IoWaitTable *table, int fd) {
    struct IoWaitEntry *entry = io_wait_table_find(table, fd);
    if (entry) {
        return entry;
    }

    if (table->count == table->capacity && !io_wait_table_grow(table)) {
        return NULL;
    }

    for (size_t i = 0; i < table->capacity; ++i) {
        if (table->entries[i].in_use) {
            continue;
        }

        entry = &table->entries[i];
        entry->fd = fd;
        entry->read_task = NULL;
        entry->write_task = NULL;
        entry->in_use = true;
        table->count++;
        return entry;
    }

    return NULL;
}

static void io_wait_table_remove_entry(struct IoWaitTable *table,
                                       struct IoWaitEntry *entry) {
    if (!entry || !entry->in_use) {
        return;
    }

    io_wait_entry_reset(entry);
    if (table->count > 0) {
        table->count--;
    }
}

static void io_wait_entry_clear_task(struct IoWaitEntry *entry,
                                     struct Task *task) {
    if (entry->read_task == task) {
        entry->read_task = NULL;
    }
    if (entry->write_task == task) {
        entry->write_task = NULL;
    }
}

// Caller must hold scheduler_mutex while reconciling wait-table state with the
// poller registration for this fd.
static bool update_poller_interest_locked(int fd, uint32_t old_events,
                                          uint32_t new_events) {
    return croutine_poller_set(fd, old_events, new_events);
}

// Caller must hold scheduler_mutex before moving a task back onto the runnable
// queue.
static bool enqueue_runnable_task_locked(struct Task *task) {
    task->scheduler.state = TASK_RUNNABLE;
    task->io_wait.fd = -1;
    task->io_wait.events = CROUTINE_IO_EVENT_NONE;
    return task_queue_push(&runnable_tasks, task);
}

// Caller must hold scheduler_mutex while waking tasks and updating the shared
// wait-table / poller state for the fd.
static size_t wake_waiting_tasks_locked(int fd, uint32_t ready_events,
                                        bool wake_success) {
    struct IoWaitEntry *entry = io_wait_table_find(&io_waits, fd);
    if (!entry) {
        return 0;
    }

    struct Task *tasks_to_wake[2] = {NULL, NULL};
    size_t wake_count = 0;
    const uint32_t old_events = io_wait_entry_events(entry);

    if ((ready_events & CROUTINE_IO_EVENT_READ) != 0 && entry->read_task) {
        struct Task *task = entry->read_task;
        io_wait_entry_clear_task(entry, task);
        tasks_to_wake[wake_count++] = task;
    }

    if ((ready_events & CROUTINE_IO_EVENT_WRITE) != 0 && entry->write_task) {
        struct Task *task = entry->write_task;
        io_wait_entry_clear_task(entry, task);
        if (wake_count == 0 || tasks_to_wake[0] != task) {
            tasks_to_wake[wake_count++] = task;
        }
    }

    const uint32_t new_events = io_wait_entry_events(entry);
    if (!update_poller_interest_locked(fd, old_events, new_events)) {
        worker_fail("Failed to update poller interest while waking task");
    }
    if (new_events == CROUTINE_IO_EVENT_NONE) {
        io_wait_table_remove_entry(&io_waits, entry);
    }

    for (size_t i = 0; i < wake_count; ++i) {
        tasks_to_wake[i]->io_wait.wake_success = wake_success;
        if (!enqueue_runnable_task_locked(tasks_to_wake[i])) {
            worker_fail("Failed to enqueue runnable task");
        }
    }

    if (wake_count > 0) {
        pthread_cond_broadcast(&scheduler_cond);
    }

    return wake_count;
}

void worker_forget_fd(int fd) {
    if (fd < 0) {
        return;
    }

    pthread_mutex_lock(&scheduler_mutex);
    const bool should_wake_poller =
        wake_waiting_tasks_locked(
            fd, CROUTINE_IO_EVENT_READ | CROUTINE_IO_EVENT_WRITE, false) > 0 &&
        poller_waiter_active;
    pthread_mutex_unlock(&scheduler_mutex);
    if (should_wake_poller) {
        (void)croutine_poller_wake();
    }
}

static void runtime_init_impl(void) {
    if (!task_queue_init(&runnable_tasks, INITIAL_RUN_QUEUE_CAPACITY)) {
        runtime_init_ok = false;
        return;
    }

    if (!io_wait_table_init(&io_waits, INITIAL_IO_WAIT_CAPACITY)) {
        task_queue_deinit(&runnable_tasks);
        runtime_init_ok = false;
        return;
    }

    if (!croutine_poller_init()) {
        io_wait_table_deinit(&io_waits);
        task_queue_deinit(&runnable_tasks);
        runtime_init_ok = false;
        return;
    }

    runtime_init_ok = true;
}

static void task_finished(void) {
    if (!croutine_current_task) {
        return;
    }

    croutine_current_task->scheduler.state = TASK_FINISHED;
    worker.yielded_state = TASK_FINISHED;
    worker.returned_with_scheduler_lock = false;
    if (croutine_current_task->scheduler.is_main) {
        pthread_mutex_lock(&main_mutex);
        main_finished = true;
        pthread_cond_broadcast(&main_cond);
        pthread_mutex_unlock(&main_mutex);

        pthread_mutex_lock(&scheduler_mutex);
        scheduler_main_finished = true;
        pthread_cond_broadcast(&scheduler_cond);
        pthread_mutex_unlock(&scheduler_mutex);
        (void)croutine_poller_wake();
    }

    struct Task *task = croutine_current_task;
    start_switch_from_task(task);
    context_switch(&task->context, &worker.context);
}

void task_entrypoint(void) {
    finish_switch_to_task(croutine_current_task);
    croutine_task_fn fn = get_task_body();
    void *arg = get_task_arg();
    fn(arg);
    task_finished();
}

bool worker_init(void) {
    pthread_once(&runtime_init_once, runtime_init_impl);
    return runtime_init_ok;
}

void worker_configure_stack_size(size_t stack_size) {
    configured_stack_size =
        stack_size ? stack_size : CROUTINE_DEFAULT_STACK_SIZE;
}

void worker_request_shutdown(void) {
    if (!runtime_init_ok) {
        return;
    }

    pthread_mutex_lock(&scheduler_mutex);
    shutdown_requested = true;
    croutine_channel_shutdown_all_locked();
    pthread_cond_broadcast(&scheduler_cond);
    pthread_mutex_unlock(&scheduler_mutex);
    pthread_mutex_lock(&main_mutex);
    main_finished = true;
    pthread_cond_broadcast(&main_cond);
    pthread_mutex_unlock(&main_mutex);
    (void)croutine_poller_wake();
}

void worker_deinit(void) {
    if (!runtime_init_ok) {
        return;
    }

    pthread_mutex_lock(&scheduler_mutex);

    struct Task *task = NULL;
    while (task_queue_pop(&runnable_tasks, &task)) {
        cleanup_task(task);
    }

    for (size_t i = 0; i < io_waits.capacity; ++i) {
        struct IoWaitEntry *entry = &io_waits.entries[i];
        if (!entry->in_use) {
            continue;
        }

        struct Task *read_task = entry->read_task;
        struct Task *write_task = entry->write_task;
        io_wait_table_remove_entry(&io_waits, entry);
        cleanup_task(read_task);
        if (write_task != read_task) {
            cleanup_task(write_task);
        }
    }

    pthread_mutex_unlock(&scheduler_mutex);

    croutine_poller_deinit();
    io_wait_table_deinit(&io_waits);
    task_queue_deinit(&runnable_tasks);
    /* Keep the one-shot scheduler lock alive so closed channels can be
       explicitly destroyed after runtime shutdown. */
    runtime_init_ok = false;
}

static void worker_loop_impl(int worker_id) {
    for (;;) {
        struct Task *task = NULL;
        bool checked_ready_io = false;

        pthread_mutex_lock(&scheduler_mutex);
        for (;;) {
            if (shutdown_requested || scheduler_main_finished) {
                pthread_mutex_unlock(&scheduler_mutex);
                return;
            }

            bool have_runnable = !task_queue_is_empty(&runnable_tasks);
            if (!poller_waiter_active &&
                (!have_runnable || !checked_ready_io)) {
                struct CroutinePollEvent events[MAX_POLLER_EVENTS];
                int timeout_ms = have_runnable ? 0 : -1;
                checked_ready_io = true;

                poller_waiter_active = true;
                pthread_mutex_unlock(&scheduler_mutex);

                const int ready =
                    croutine_poller_wait(events, MAX_POLLER_EVENTS, timeout_ms);

                pthread_mutex_lock(&scheduler_mutex);
                poller_waiter_active = false;
                pthread_cond_broadcast(&scheduler_cond);

                if (shutdown_requested || scheduler_main_finished) {
                    pthread_mutex_unlock(&scheduler_mutex);
                    return;
                }

                if (ready < 0) {
                    pthread_mutex_unlock(&scheduler_mutex);
                    worker_fail("Poller wait failed");
                }

                for (int i = 0; i < ready; ++i) {
                    (void)wake_waiting_tasks_locked(events[i].fd,
                                                    events[i].events, true);
                }
                continue;
            }

            if (task_queue_pop(&runnable_tasks, &task)) {
                break;
            }

            pthread_cond_wait(&scheduler_cond, &scheduler_mutex);
        }
        pthread_mutex_unlock(&scheduler_mutex);

        croutine_current_task = task;

        switch_from_scheduler(task);

        croutine_current_task = NULL;
        // Use the state captured at yield time instead of re-reading task state
        // after a cross-worker resume.
        const enum TaskState yielded_state = worker.yielded_state;
        const bool returned_with_scheduler_lock =
            worker.returned_with_scheduler_lock;
        worker.returned_with_scheduler_lock = false;

        if (returned_with_scheduler_lock) {
            pthread_mutex_unlock(&scheduler_mutex);
        }

        pthread_mutex_lock(&scheduler_mutex);
        const bool should_shutdown = shutdown_requested;
        pthread_mutex_unlock(&scheduler_mutex);

        if (should_shutdown && yielded_state == TASK_RUNNABLE) {
            cleanup_task(task);
            continue;
        }

        switch (yielded_state) {
        case TASK_RUNNABLE:
            pthread_mutex_lock(&scheduler_mutex);
            if (!task_queue_push(&runnable_tasks, task)) {
                pthread_mutex_unlock(&scheduler_mutex);
                worker_fail("Failed to requeue task");
            }
            pthread_cond_signal(&scheduler_cond);
            pthread_mutex_unlock(&scheduler_mutex);
            break;
        case TASK_WAITING_IO:
        case TASK_WAITING_CHANNEL:
            break;
        case TASK_FINISHED:
            task_cleanup(task);
            free(task);
            break;
        default:
            worker_fail("Unknown task state");
        }
    }

    (void)worker_id;
}

void *worker_loop(void *arg) {
    const int worker_id = (int)(intptr_t)arg;
    worker_loop_impl(worker_id);
    return NULL;
}

croutine_io_wait_result worker_park_current_on_io(int fd, uint32_t events) {
    if (!croutine_current_task || events == CROUTINE_IO_EVENT_NONE) {
        return CROUTINE_IO_WAIT_FAILED;
    }

    // `context_switch()` may suspend here and resume this frame on a different
    // worker thread, so snapshot task/worker-owned state before touching TLS.
    struct Task *task = croutine_current_task;
    struct Context *worker_context = &worker.context;

    pthread_mutex_lock(&scheduler_mutex);

    if (shutdown_requested) {
        pthread_mutex_unlock(&scheduler_mutex);
        return CROUTINE_IO_WAIT_FAILED;
    }

    struct IoWaitEntry *entry = io_wait_table_get_or_insert(&io_waits, fd);
    if (!entry) {
        pthread_mutex_unlock(&scheduler_mutex);
        return CROUTINE_IO_WAIT_FAILED;
    }

    if (((events & CROUTINE_IO_EVENT_READ) != 0 && entry->read_task) ||
        ((events & CROUTINE_IO_EVENT_WRITE) != 0 && entry->write_task)) {
        pthread_mutex_unlock(&scheduler_mutex);
        return CROUTINE_IO_WAIT_FAILED;
    }

    const uint32_t old_events = io_wait_entry_events(entry);
    const uint32_t new_events = old_events | events;
    if (!update_poller_interest_locked(fd, old_events, new_events)) {
        if (old_events == CROUTINE_IO_EVENT_NONE &&
            io_wait_entry_events(entry) == CROUTINE_IO_EVENT_NONE) {
            io_wait_table_remove_entry(&io_waits, entry);
        }
        pthread_mutex_unlock(&scheduler_mutex);
        return CROUTINE_IO_WAIT_FAILED;
    }

    if ((events & CROUTINE_IO_EVENT_READ) != 0) {
        entry->read_task = task;
    }
    if ((events & CROUTINE_IO_EVENT_WRITE) != 0) {
        entry->write_task = task;
    }

    task->scheduler.state = TASK_WAITING_IO;
    task->io_wait.fd = fd;
    task->io_wait.events = events;
    task->io_wait.wake_success = false;
    worker.yielded_state = TASK_WAITING_IO;
    worker.returned_with_scheduler_lock = true;

    start_switch_from_task(task);
    context_switch(&task->context, worker_context);
    finish_switch_to_task(task);
    return task->io_wait.wake_success ? CROUTINE_IO_WAIT_READY
                                      : CROUTINE_IO_WAIT_CLOSED;
}

void worker_yield(void) {
    worker.yielded_state = TASK_RUNNABLE;
    worker.returned_with_scheduler_lock = false;
    struct Task *task = croutine_current_task;
    start_switch_from_task(task);
    context_switch(&task->context, &worker.context);
    finish_switch_to_task(task);
}

void worker_scheduler_lock(void) { pthread_mutex_lock(&scheduler_mutex); }

void worker_scheduler_unlock(void) { pthread_mutex_unlock(&scheduler_mutex); }

bool worker_shutdown_requested_locked(void) { return shutdown_requested; }

struct Task *worker_current_task(void) { return croutine_current_task; }

bool worker_park_current_on_channel_locked(void) {
    if (!croutine_current_task || shutdown_requested) {
        pthread_mutex_unlock(&scheduler_mutex);
        return false;
    }

    struct Task *task = croutine_current_task;
    struct Context *worker_context = &worker.context;

    task->scheduler.state = TASK_WAITING_CHANNEL;
    task->channel_wait.wake_success = false;
    worker.yielded_state = TASK_WAITING_CHANNEL;
    worker.returned_with_scheduler_lock = true;

    start_switch_from_task(task);
    context_switch(&task->context, worker_context);
    finish_switch_to_task(task);
    return task->channel_wait.wake_success;
}

bool worker_wake_task_locked(struct Task *task, bool success) {
    if (!task || task->scheduler.state != TASK_WAITING_CHANNEL) {
        return false;
    }

    task->channel_wait.wake_success = success;
    if (!enqueue_runnable_task_locked(task)) {
        return false;
    }

    pthread_cond_signal(&scheduler_cond);
    return true;
}

bool worker_spawn(croutine_task_fn fn, const void *arg, size_t arg_size,
                  bool copy_arg, bool is_main) {
    if (!worker_init()) {
        fprintf(stderr, "Failed to initialize worker/runtime\n");
        return false;
    }

    struct Task *task = calloc(1, sizeof(struct Task));
    if (!task) {
        fprintf(stderr, "Failed to allocate task\n");
        return false;
    }

    uint8_t *stack = create_stack(configured_stack_size);
    if (!stack) {
        fprintf(stderr, "Failed to create stack for task\n");
        free(task);
        return false;
    }

    uint64_t stack_top = (uint64_t)stack + configured_stack_size;
    void *task_arg = (void *)arg;
    if (copy_arg && arg_size > 0) {
        task_arg = malloc(arg_size);
        if (!task_arg) {
            destroy_stack(stack, configured_stack_size);
            free(task);
            return false;
        }
        memcpy(task_arg, arg, arg_size);
    }

    task->fn = fn;
    task->arg = task_arg;
    task->arg_size = arg_size;
    task->owns_arg = copy_arg && arg_size > 0;
    task->stack = stack;
    task->stack_size = configured_stack_size;
    task->scheduler.state = TASK_RUNNABLE;
    task->scheduler.queued = false;
    task->scheduler.is_main = is_main;
    task->io_wait.fd = -1;
    task->io_wait.events = CROUTINE_IO_EVENT_NONE;
    task->io_wait.wake_success = false;
    task->channel_wait.wake_success = false;
    create_context(&task->context, fn, task_arg, stack_top);

    if (is_main) {
        pthread_mutex_lock(&main_mutex);
        if (main_spawned) {
            pthread_mutex_unlock(&main_mutex);
            fprintf(stderr, "Main task already spawned\n");
            task_cleanup(task);
            free(task);
            return false;
        }
        main_spawned = true;
        pthread_mutex_unlock(&main_mutex);
    }

    pthread_mutex_lock(&scheduler_mutex);
    if (shutdown_requested) {
        pthread_mutex_unlock(&scheduler_mutex);
        if (is_main) {
            pthread_mutex_lock(&main_mutex);
            main_spawned = false;
            pthread_mutex_unlock(&main_mutex);
        }
        task_cleanup(task);
        free(task);
        return false;
    }
    const bool should_wake_poller = poller_waiter_active;
    const bool ok = task_queue_push(&runnable_tasks, task);
    if (ok) {
        pthread_cond_signal(&scheduler_cond);
    }
    pthread_mutex_unlock(&scheduler_mutex);
    if (!ok) {
        fprintf(stderr, "Failed to push task to queue\n");
        if (is_main) {
            pthread_mutex_lock(&main_mutex);
            main_spawned = false;
            pthread_mutex_unlock(&main_mutex);
        }
        task_cleanup(task);
        free(task);
        return false;
    }

    if (should_wake_poller) {
        (void)croutine_poller_wake();
    }
    return true;
}

void worker_reset_main_state(void) {
    pthread_mutex_lock(&main_mutex);
    main_finished = false;
    main_spawned = false;
    main_exit_code = 0;
    pthread_mutex_unlock(&main_mutex);

    if (runtime_init_ok) {
        pthread_mutex_lock(&scheduler_mutex);
        shutdown_requested = false;
        scheduler_main_finished = false;
        poller_waiter_active = false;
        pthread_mutex_unlock(&scheduler_mutex);
    } else {
        shutdown_requested = false;
        scheduler_main_finished = false;
        poller_waiter_active = false;
    }
}

int worker_wait_for_main(void) {
    pthread_mutex_lock(&main_mutex);
    while (!main_finished) {
        pthread_cond_wait(&main_cond, &main_mutex);
    }
    int exit_code = main_exit_code;
    pthread_mutex_unlock(&main_mutex);
    return exit_code;
}

void croutine_runtime_set_exit_code(int code) {
    pthread_mutex_lock(&main_mutex);
    main_exit_code = code;
    pthread_mutex_unlock(&main_mutex);
}

bool croutine_spawn(croutine_task_fn fn, void *arg) {
    return fn && worker_spawn(fn, arg, 0, false, false);
}

bool croutine_spawn_copy(croutine_task_fn fn, const void *arg,
                         size_t arg_size) {
    if (!fn || (arg_size > 0 && !arg)) {
        return false;
    }
    return worker_spawn(fn, arg, arg_size, true, false);
}

void croutine_yield(void) {
    if (croutine_current_task) {
        worker_yield();
    }
}
