#include <croutine/mn.h>
#include <croutine/worker.h>
#include <stdio.h>

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
    for (int i = 0; i < 100000; ++i) {
        spawn(f);
    }

    spawn(g);
    spawn(h);

    begin_runtime();
}
