#ifndef CROUTINE_IO_H
#define CROUTINE_IO_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int croutine_fd_set_nonblocking(int fd);
ssize_t croutine_read(int fd, void *buffer, size_t count);
ssize_t croutine_write(int fd, const void *buffer, size_t count);
int croutine_accept(int fd, struct sockaddr *address, socklen_t *address_len);

#ifdef __cplusplus
}
#endif

#endif
