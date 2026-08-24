#include "internal/task.h"

#include <string.h>

int main(void) {
    struct TaskQueue queue;
    struct Task first;
    struct Task second;
    struct Task *popped = NULL;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));

    if (!task_queue_init(&queue, 1)) {
        return 1;
    }
    if (!task_queue_push(&queue, &first) ||
        !task_queue_push(&queue, &first) || queue.count != 1) {
        return 1;
    }
    if (!task_queue_push(&queue, &second) || queue.capacity < 2) {
        return 1;
    }
    if (!task_queue_pop(&queue, &popped) || popped != &first || first.scheduler.queued) {
        return 1;
    }
    if (!task_queue_pop(&queue, &popped) || popped != &second ||
        !task_queue_is_empty(&queue)) {
        return 1;
    }
    task_queue_deinit(&queue);
    return 0;
}
