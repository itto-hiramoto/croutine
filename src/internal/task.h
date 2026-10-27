#ifndef CROUTINE_TASK_H
#define CROUTINE_TASK_H

#if defined(__x86_64__) || defined(_M_X64)
#include "internal/arch/x86_64.h"
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "internal/arch/aarch64.h"
#else
#error "Unsupported architecture for croutine runtime"
#endif
#include <croutine/runtime.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CROUTINE_DEFAULT_STACK_SIZE (256u * 1024u)

enum TaskState {
    TASK_RUNNABLE,
    TASK_WAITING_IO,
    TASK_WAITING_CHANNEL,
    TASK_FINISHED,
};

enum CroutineIoEvent {
    CROUTINE_IO_EVENT_NONE = 0,
    CROUTINE_IO_EVENT_READ = 1u << 0,
    CROUTINE_IO_EVENT_WRITE = 1u << 1,
};

// Scheduler-owned execution state for queueing and task lifecycle.
struct TaskSchedulerState {
    enum TaskState state;
    bool queued;
    bool is_main;
};

// Scheduler-owned I/O wait state for the task's most recent park.
struct TaskIoWaitState {
    int fd;
    uint32_t events;
    bool wake_success;
};
struct Task;

// Set by the waker (channel runtime) before a parked task is re-enqueued.
// `true` means the wait completed normally and the waiter (channel-side) has
// populated its result fields; `false` signals shutdown or a closed channel.
struct TaskChannelWaitState {
    bool wake_success;
};
struct Task {
    struct Context context;
    croutine_task_fn fn;
    void *arg;
    size_t arg_size;
    bool owns_arg;
    uint8_t *stack;
    size_t stack_size;
    void *asan_fake_stack;
    const void *asan_scheduler_stack;
    size_t asan_scheduler_stack_size;
    void *tsan_fiber;
    struct TaskSchedulerState scheduler;
    struct TaskIoWaitState io_wait;
    struct TaskChannelWaitState channel_wait;
};

struct TaskQueue {
    struct Task **tasks;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
};

bool task_queue_init(struct TaskQueue *queue, size_t capacity);
void task_queue_deinit(struct TaskQueue *queue);
bool task_queue_is_empty(const struct TaskQueue *queue);
bool task_queue_is_full(const struct TaskQueue *queue);
bool task_queue_push(struct TaskQueue *queue, struct Task *task);
bool task_queue_pop(struct TaskQueue *queue, struct Task **task);

uint8_t *create_stack(size_t stack_size);
void destroy_stack(uint8_t *stack, size_t stack_size);
void task_cleanup(struct Task *task);

#endif // CROUTINE_TASK_H
