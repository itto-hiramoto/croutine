#include <croutine/task.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static struct TaskQueue tasks;
static pthread_mutex_t tasks_mutex;
static pthread_once_t runtime_init_once = PTHREAD_ONCE_INIT;
static bool runtime_init_ok = false;

struct Worker {
    struct Task *current_task;
    struct Context context;
};

_Thread_local struct Worker worker;

static void runtime_init_impl(void) {
    if (pthread_mutex_init(&tasks_mutex, NULL) != 0) {
        runtime_init_ok = false;
        return;
    }

    if (!task_queue_init(&tasks, 1024)) {
        pthread_mutex_destroy(&tasks_mutex);
        runtime_init_ok = false;
        return;
    }

    runtime_init_ok = true;
}

void task_finished(void) {
    if (!worker.current_task) {
        return;
    }

    worker.current_task->finished = true;

    // Switch back to scheduler
    context_switch(&worker.current_task->context, &worker.context);
}

void task_entrypoint(void) {
    void (*fn)(void) = get_task_body();
    fn();
    task_finished();
}

bool worker_init(void) {
    pthread_once(&runtime_init_once, runtime_init_impl);
    return runtime_init_ok;
}

void worker_deinit(void) {
    if (!runtime_init_ok) {
        return;
    }
    pthread_mutex_lock(&tasks_mutex);
    task_queue_deinit(&tasks);
    pthread_mutex_unlock(&tasks_mutex);
    pthread_mutex_destroy(&tasks_mutex);
}

static void worker_loop_impl(int worker_id) {
    for (;;) {
        struct Task task;
        pthread_mutex_lock(&tasks_mutex);
        const bool ok = task_queue_pop(&tasks, &task);
        pthread_mutex_unlock(&tasks_mutex);
        if (!ok) {
            break;
        }

        // Store pointer to local task variable
        // This is safe because context_switch returns to the same stack frame
        worker.current_task = &task;

        context_switch(&worker.context, &task.context);

        // After context switch returns, task is still valid in this stack frame
        if (!task.finished) {
            // Push the task back to the queue
            pthread_mutex_lock(&tasks_mutex);
            (void)task_queue_push(&tasks, &task);
            pthread_mutex_unlock(&tasks_mutex);
        }
    }

    printf("Worker %d exiting\n", worker_id);
}

void *worker_loop(void *arg) {
    const int worker_id = (int)(intptr_t)arg;
    worker_loop_impl(worker_id);
    return NULL;
}

// Yield the current green thread
void yield(void) {
    context_switch(&worker.current_task->context, &worker.context);
}

// Spawn a new green thread
void spawn(void (*fn)(void)) {
    if (!worker_init()) {
        fprintf(stderr, "Failed to initialize worker/runtime\n");
        abort();
    }

    uint8_t *stack = create_stack();
    if (!stack) {
        fprintf(stderr, "Failed to create stack for task\n");
        worker_deinit();
        abort();
    }

    // Stack top is just below the guard page (stack grows downward)
    // Guard page starts at stack + STACK_SIZE
    const uint64_t stack_top = (uint64_t)stack + STACK_SIZE;

    struct Task task;
    create_context(&task.context, task_entrypoint, fn, stack_top);
    task.finished = false;
    pthread_mutex_lock(&tasks_mutex);
    const bool ok = task_queue_push(&tasks, &task);
    pthread_mutex_unlock(&tasks_mutex);
    if (!ok) {
        fprintf(stderr, "Failed to push task to queue\n");
        destroy_stack(stack);
        worker_deinit();
        abort();
    }
}
