#ifndef COMMON_H
#define COMMON_H

#include "arch/x86_64.h"
#include <stdbool.h>
#include <stddef.h>

#define STACK_SIZE (4 * 1024) // 4KB

struct Task {
    struct Context context;
    bool finished;
};

struct TaskQueue {
    struct Task *tasks;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
};

bool task_queue_init(struct TaskQueue *queue, size_t capacity);
void task_queue_deinit(struct TaskQueue *queue);
bool task_queue_is_empty(const struct TaskQueue *queue);
bool task_queue_is_full(const struct TaskQueue *queue);
bool task_queue_push(struct TaskQueue *queue, const struct Task *task);
bool task_queue_pop(struct TaskQueue *queue, struct Task *task);

uint8_t *create_stack(void);
void destroy_stack(uint8_t *stack);

#endif // COMMON_H
