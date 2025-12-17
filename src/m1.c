#include <croutine/task.h>
#include <stdio.h>
#include <stdlib.h>

struct Worker {
    struct TaskQueue tasks;
    struct Task *current_task;
    struct Context context;
};

static struct Worker worker;

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
    if (!task_queue_init(&worker.tasks, 1024)) {
        return false;
    }

    return true;
}

void worker_deinit(void) { task_queue_deinit(&worker.tasks); }

void worker_loop(void) {
    for (;;) {
        struct Task task;
        if (!task_queue_pop(&worker.tasks, &task)) {
            break;
        }

        // Store pointer to local task variable
        // This is safe because context_switch returns to the same stack frame
        worker.current_task = &task;

        context_switch(&worker.context, &task.context);

        // After context switch returns, task is still valid in this stack frame
        if (!task.finished) {
            // Push the task back to the queue
            task_queue_push(&worker.tasks, &task);
        }
    }
}

// Yield the current green thread
void yield(void) {
    context_switch(&worker.current_task->context, &worker.context);
}

// Spawn a new green thread
void spawn(void (*fn)(void)) {
    uint8_t *stack = create_stack();
    if (!stack) {
        fprintf(stderr, "Failed to create stack for task\n");
        worker_deinit();
        abort();
    }

    // Stack top is just below the guard page (stack grows downward)
    // Guard page starts at stack + STACK_SIZE
    uint64_t stack_top = (uint64_t)stack + STACK_SIZE;

    struct Task task;
    create_context(&task.context, task_entrypoint, fn, stack_top);
    task.finished = false;
    if (!task_queue_push(&worker.tasks, &task)) {
        destroy_stack(stack);
        fprintf(stderr, "Failed to push task to queue\n");
        worker_deinit();
        abort();
    }
}

void f(void) {
    printf("hello, world! from f\n");
    yield();
    printf("hello, world! from f again\n");
}

void g(void) {
    printf("hello, world! from g\n");
    yield();
    printf("hello, world! from g again\n");
}

void h(void) {
    printf("hello, world! from h\n");
    yield();
    printf("hello, world! from h again\n");
}

int main(void) {
    if (!worker_init()) {
        fprintf(stderr, "Failed to initialize worker\n");
        return 1;
    }

    for (int i = 0; i < 100000; ++i) {
        spawn(f);
    }

    spawn(g);
    spawn(h);

    worker_loop();
    worker_deinit();
}
