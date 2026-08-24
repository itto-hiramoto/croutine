#include <croutine/io.h>
#include <croutine/runtime.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int croutine_fd_set_nonblocking(int fd) {
    int flags;
    do {
        flags = fcntl(fd, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    int result;
    do {
        result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (result < 0 && errno == EINTR);
    return result;
}

ssize_t croutine_read(int fd, void *buffer, size_t count) {
    for (;;) {
        ssize_t result = read(fd, buffer, count);
        if (result >= 0) {
            return result;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        croutine_io_wait_result wait = croutine_io_wait_readable(fd);
        if (wait != CROUTINE_IO_WAIT_READY) {
            errno = wait == CROUTINE_IO_WAIT_CLOSED ? ECANCELED : ENOTSUP;
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
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        croutine_io_wait_result wait = croutine_io_wait_writable(fd);
        if (wait != CROUTINE_IO_WAIT_READY) {
            errno = wait == CROUTINE_IO_WAIT_CLOSED ? ECANCELED : ENOTSUP;
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
            int saved_errno = errno;
            close(accepted);
            errno = saved_errno;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        croutine_io_wait_result wait = croutine_io_wait_readable(fd);
        if (wait != CROUTINE_IO_WAIT_READY) {
            errno = wait == CROUTINE_IO_WAIT_CLOSED ? ECANCELED : ENOTSUP;
            return -1;
        }
    }
}
