# croutine

`croutine` is an experimental M:N threading runtime written in C. It
multiplexes lightweight user-space tasks over a pool of POSIX worker threads.

## Status

The project is under active development toward a standalone C runtime.

The current implementation provides:

- a standalone static C library with configurable workers and task stacks;
- cooperative M:N task scheduling with explicit startup and shutdown;
- Linux epoll and macOS kqueue readiness polling;
- runtime-aware non-blocking POSIX I/O helpers;
- buffered and unbuffered channels with select support; and
- x86_64 and AArch64 context switching.

The public API is experimental and may still change.

## Requirements

- CMake 3.10 or later
- A C11 compiler compatible with GCC or Clang
- A supported POSIX environment with pthreads

## Build and run

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/croutine_basic
```

See [`docs/API.md`](docs/API.md) for lifecycle and ownership rules.

## Direction

Development is focused on hardening the standalone runtime and extending its
portable scheduler-aware synchronization primitives.

The runtime originated as an experiment for the Kaede programming language.
Kaede's newer runtime contains later scheduler and I/O work that can inform
croutine's standalone implementation.
