#ifndef CROUTINE_CHANNEL_H
#define CROUTINE_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct croutine_channel croutine_channel;

typedef enum croutine_channel_send_result {
    CROUTINE_CHANNEL_SEND_OK = 0,
    CROUTINE_CHANNEL_SEND_CLOSED = 1,
} croutine_channel_send_result;

typedef enum croutine_channel_recv_result {
    CROUTINE_CHANNEL_RECV_VALUE = 0,
    CROUTINE_CHANNEL_RECV_EMPTY = 1,
    CROUTINE_CHANNEL_RECV_CLOSED = 2,
} croutine_channel_recv_result;

typedef enum croutine_select_op {
    CROUTINE_SELECT_OP_SEND = 0,
    CROUTINE_SELECT_OP_RECV = 1,
} croutine_select_op;

typedef enum croutine_select_status {
    CROUTINE_SELECT_STATUS_VALUE = 0,
    CROUTINE_SELECT_STATUS_CLOSED = 1,
    CROUTINE_SELECT_STATUS_SENT = 2,
} croutine_select_status;

typedef struct croutine_select_case {
    croutine_channel *channel;
    croutine_select_op op;
    void *value;
    croutine_select_status status;
} croutine_select_case;

#define CROUTINE_SELECT_DEFAULT_INDEX (-1)

croutine_channel *croutine_channel_create(size_t elem_size, size_t capacity);
/* The channel must be closed and have no outstanding operations. */
bool croutine_channel_destroy(croutine_channel *channel);
croutine_channel_send_result croutine_channel_send(croutine_channel *channel,
                                                    const void *value);
bool croutine_channel_try_send(croutine_channel *channel, const void *value);
croutine_channel_recv_result croutine_channel_recv(croutine_channel *channel,
                                                    void *out);
bool croutine_channel_try_recv(croutine_channel *channel, void *out);
void croutine_channel_close(croutine_channel *channel);
bool croutine_channel_is_closed(croutine_channel *channel);
int croutine_select(croutine_select_case *cases, size_t count,
                    bool has_default);

#ifdef __cplusplus
}
#endif

#endif
