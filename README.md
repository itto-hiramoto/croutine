# croutine

**croutine** is an experimental **M:N threading library written in C**.

The project implements a lightweight threading system similar in spirit to **Go's goroutines**, but implemented in C — hence the name **"croutine"**.

> ⚠️ This repository is now archived.

The implementation has been integrated into the **Kaede programming language runtime**.

## Status

This repository is no longer maintained as a standalone project.

The implementation now lives in the Kaede runtime:

[https://github.com/itto-hiramoto/kaede/tree/main/library/runtime](https://github.com/itto-hiramoto/kaede/tree/main/library/runtime)

Future development will continue there.

## About

`croutine` provides a lightweight threading system based on an **M:N threading model**, where many user-level threads are multiplexed over a smaller number of OS threads.

The project was originally developed to experiment with runtime scheduling techniques for the **Kaede programming language**.

As the Kaede runtime matured, the codebase was merged into the Kaede repository.
