#include <croutine/io.h>
#include <croutine/runtime.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

// A task may resume on another worker after waiting, so errno is accessed
// only from non-inlined frames that resolve the running thread's errno.
static __attribute__((noinline)) int current_errno(void) { return errno; }

static __attribute__((noinline)) void set_errno(int error) { errno = error; }

static int wait_errno(croutine_io_wait_result wait) {
    return wait == CROUTINE_IO_WAIT_CLOSED ? ECANCELED : ENOTSUP;
}

int croutine_fd_set_nonblocking(int fd) {
    int flags;
    do {
        flags = fcntl(fd, F_GETFL);
    } while (flags < 0 && current_errno() == EINTR);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    int result;
    do {
        result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (result < 0 && current_errno() == EINTR);
    return result;
}

ssize_t croutine_read(int fd, void *buffer, size_t count) {
    for (;;) {
        ssize_t result = read(fd, buffer, count);
        if (result >= 0) {
            return result;
        }
        const int error = current_errno();
        if (error == EINTR) {
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            return -1;
        }
        croutine_io_wait_result wait = croutine_io_wait_readable(fd);
        if (wait != CROUTINE_IO_WAIT_READY) {
            set_errno(wait_errno(wait));
            return -1;
        }
    }
}

ssize_t croutine_write(int fd, const void *buffer, size_t count) {
    for (;;) {
        ssize_t result = write(fd, buffer, count);
        if (result >= 0) {
            return result;
        }
        const int error = current_errno();
        if (error == EINTR) {
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            return -1;
        }
        croutine_io_wait_result wait = croutine_io_wait_writable(fd);
        if (wait != CROUTINE_IO_WAIT_READY) {
            set_errno(wait_errno(wait));
            return -1;
        }
    }
}

int croutine_accept(int fd, struct sockaddr *address, socklen_t *address_len) {
    for (;;) {
        int accepted = accept(fd, address, address_len);
        if (accepted >= 0) {
            if (croutine_fd_set_nonblocking(accepted) == 0) {
                return accepted;
            }
            const int error = current_errno();
            close(accepted);
            set_errno(error);
            return -1;
        }
        const int error = current_errno();
        if (error == EINTR) {
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            return -1;
        }
        croutine_io_wait_result wait = croutine_io_wait_readable(fd);
        if (wait != CROUTINE_IO_WAIT_READY) {
            set_errno(wait_errno(wait));
            return -1;
        }
    }
}
