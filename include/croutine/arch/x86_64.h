#ifndef ARCH_X86_64_H
#define ARCH_X86_64_H

#include <stdint.h>

struct Context {
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
};

void create_context(struct Context *context, void (*wrapper)(void),
                    void (*fn)(void), uint64_t stack_top);

void (*get_task_body(void))(void);

void context_switch(struct Context *old, struct Context *new);

#endif // ARCH_X86_64_H
