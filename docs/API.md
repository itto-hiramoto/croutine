# croutine C API

`croutine` is a process-wide, one-shot runtime. Call
`croutine_runtime_init`, then `croutine_runtime_run`, and finally
`croutine_runtime_shutdown`. Worker count and per-task usable stack size are
selected with `croutine_runtime_config`; zero selects the documented default.
Stacks are `mmap` allocations with a protected guard page on POSIX systems.

`croutine_spawn` borrows its argument pointer. The caller must keep the pointed
object alive until the task completes. `croutine_spawn_copy` copies exactly
`arg_size` bytes and the runtime releases that copy when it destroys the task.

Channels copy fixed-size values into their own buffer or directly between a
sender and receiver. A channel is caller-owned. Close it before destruction;
`croutine_channel_destroy` returns false if the channel is open or still has
waiters. Shutdown closes every registered channel and cancels its waiters, so a
caller may destroy such channels after `croutine_runtime_shutdown`.
`croutine_select` randomizes immediately-ready cases and can either park the
current task or report `CROUTINE_SELECT_DEFAULT_INDEX` for a default arm.

File descriptors must be non-blocking before runtime-aware I/O. The readiness
wait functions only report readiness: they do not guarantee that a following
system call completes. Callers must retry after `EAGAIN`/`EWOULDBLOCK`, or use
the retrying `croutine_read`, `croutine_write`, and `croutine_accept` helpers.
Call `croutine_io_forget_fd` before `close`; it removes poller interest and wakes
all tasks parked on that descriptor, preventing descriptor-number reuse from
delivering stale readiness.

Tests are registered with CTest. Configure with
`-DCROUTINE_SANITIZER=address` or `undefined` instruments the runtime with the
corresponding compiler sanitizer.

The library does not install process-wide signal handlers. Scheduler-aware
mutex and wait-group APIs are intentionally deferred: blocking worker pthreads
or polling with repeated yields would violate the parking model used by I/O and
channels, so those primitives should use explicit scheduler wait queues when
added.
