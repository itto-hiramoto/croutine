#include "internal/task.h"
#include "internal/worker.h"
#include <croutine/channel.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Generic channel waiter (regular send/recv and select cases share one queue).
// -----------------------------------------------------------------------------

enum ChannelWaiterKind {
    CW_REGULAR = 0,
    CW_SELECT = 1,
};

struct CroutineSelectState;
struct croutine_channel;

struct ChannelWaiter {
    int kind; // ChannelWaiterKind
    int op;   // CroutineSelectOp (SEND/RECV)
    struct ChannelWaiter *prev;
    struct ChannelWaiter *next;
    struct croutine_channel
        *channel;      // queue this waiter is parked on (or NULL once removed)
    struct Task *task; // task to wake
    void *value_slot;  // send: source; recv: destination
    struct CroutineSelectState *state; // NULL for regular waiters
    uint32_t case_index;               // for select waiters
};

struct ChannelWaitQueue {
    struct ChannelWaiter *head;
    struct ChannelWaiter *tail;
};

struct croutine_channel {
    size_t elem_size;
    // 0 means unbuffered: `buffer` stays NULL and every send has to hand off
    // directly to a waiting receiver, so try_send_locked can only succeed
    // through wake_waiting_receiver_locked.
    size_t capacity;
    bool closed;
    // Ring buffer of `capacity` slots, NULL when unbuffered. `head` is the next
    // slot to read and `tail` the next slot to write, both wrapping modulo
    // `capacity`; `len` tells full from empty when the two meet. Note that
    // `head`/`tail` here are ring positions, unlike the list endpoints of the
    // same name in ChannelWaitQueue above.
    uint8_t *buffer;
    size_t len;
    size_t head;
    size_t tail;
    struct ChannelWaitQueue send_waiters;
    struct ChannelWaitQueue recv_waiters;
    struct croutine_channel *registry_prev;
    struct croutine_channel *registry_next;
};

static struct croutine_channel *channel_registry;

// Shared by every waiter registered for one `select` evaluation.
//
// `done` is read under the scheduler lock in pop_live_waiter_locked and
// wake_all_as_closed_locked, and written under it in complete_waiter_locked,
// which is what lets it be a plain bool rather than an atomic. The chosen-case
// fields are written under the lock too, then read by the owning task after it
// has unlinked every waiter, at which point no writer remains.
struct CroutineSelectState {
    bool done;
    int32_t chosen_index;
    int32_t chosen_status;
};

static void wait_queue_push_tail(struct ChannelWaitQueue *q,
                                 struct ChannelWaiter *w,
                                 struct croutine_channel *channel) {
    w->prev = q->tail;
    w->next = NULL;
    w->channel = channel;
    if (q->tail) {
        q->tail->next = w;
    } else {
        q->head = w;
    }
    q->tail = w;
}

static struct ChannelWaiter *wait_queue_pop_head(struct ChannelWaitQueue *q) {
    struct ChannelWaiter *w = q->head;
    if (!w) {
        return NULL;
    }
    q->head = w->next;
    if (q->head) {
        q->head->prev = NULL;
    } else {
        q->tail = NULL;
    }
    w->prev = NULL;
    w->next = NULL;
    w->channel = NULL;
    return w;
}

static void wait_queue_unlink(struct ChannelWaitQueue *q,
                              struct ChannelWaiter *w) {
    if (w->prev) {
        w->prev->next = w->next;
    } else if (q->head == w) {
        q->head = w->next;
    }
    if (w->next) {
        w->next->prev = w->prev;
    } else if (q->tail == w) {
        q->tail = w->prev;
    }
    w->prev = NULL;
    w->next = NULL;
    w->channel = NULL;
}

// -----------------------------------------------------------------------------
// Buffer helpers
// -----------------------------------------------------------------------------

static uint8_t *buffer_slot(struct croutine_channel *channel, size_t index) {
    if (!channel->buffer || channel->elem_size == 0) {
        return channel->buffer;
    }
    return channel->buffer + (index * channel->elem_size);
}

static void copy_value(void *dst, const void *src, size_t len) {
    if (len == 0) {
        return;
    }
    memcpy(dst, src, len);
}

static void buffer_push(struct croutine_channel *channel, const void *value) {
    copy_value(buffer_slot(channel, channel->tail), value, channel->elem_size);
    channel->tail = (channel->tail + 1) % channel->capacity;
    channel->len++;
}

static void buffer_pop(struct croutine_channel *channel, void *out) {
    copy_value(out, buffer_slot(channel, channel->head), channel->elem_size);
    channel->head = (channel->head + 1) % channel->capacity;
    channel->len--;
}

// -----------------------------------------------------------------------------
// Wake helpers: pop one waiter and complete it. For select waiters that have
// already been claimed by another case, skip and continue popping.
// -----------------------------------------------------------------------------

// Pop the next waiter still eligible for waking (regular, or select whose
// shared state isn't `done`). Returns NULL if the queue is exhausted.
static struct ChannelWaiter *
pop_live_waiter_locked(struct ChannelWaitQueue *queue) {
    for (;;) {
        struct ChannelWaiter *w = wait_queue_pop_head(queue);
        if (!w) {
            return NULL;
        }
        if (w->kind == CW_SELECT && w->state && w->state->done) {
            // Already fired via another case; drop and continue.
            continue;
        }
        return w;
    }
}

// Complete `waiter` and wake its task. `status` is recorded only for select
// waiters, which need to know which case fired and how; a regular send/recv
// reads its outcome from `wake_success` instead. Callers are responsible for
// any value transfer that must happen before the task can read its slot.
static void complete_waiter_locked(struct ChannelWaiter *waiter,
                                   int32_t status) {
    if (waiter->kind == CW_SELECT) {
        waiter->state->done = true;
        waiter->state->chosen_index = (int32_t)waiter->case_index;
        waiter->state->chosen_status = status;
    }
    if (!worker_wake_task_locked(waiter->task, true)) {
        worker_fail("channel: could not schedule a woken task");
    }
}

static bool wake_waiting_receiver_locked(struct croutine_channel *channel,
                                         const void *value) {
    struct ChannelWaiter *receiver =
        pop_live_waiter_locked(&channel->recv_waiters);
    if (!receiver) {
        return false;
    }
    copy_value(receiver->value_slot, value, channel->elem_size);
    complete_waiter_locked(receiver, CROUTINE_SELECT_STATUS_VALUE);
    return true;
}

static bool wake_waiting_sender_direct_locked(struct croutine_channel *channel,
                                              void *out) {
    struct ChannelWaiter *sender =
        pop_live_waiter_locked(&channel->send_waiters);
    if (!sender) {
        return false;
    }
    copy_value(out, sender->value_slot, channel->elem_size);
    complete_waiter_locked(sender, CROUTINE_SELECT_STATUS_SENT);
    return true;
}

static bool buffer_one_waiting_sender_locked(struct croutine_channel *channel) {
    if (channel->capacity == 0 || channel->len >= channel->capacity) {
        return false;
    }
    struct ChannelWaiter *sender =
        pop_live_waiter_locked(&channel->send_waiters);
    if (!sender) {
        return false;
    }
    buffer_push(channel, sender->value_slot);
    complete_waiter_locked(sender, CROUTINE_SELECT_STATUS_SENT);
    return true;
}

// Drain `queue`, completing select waiters with CLOSED and waking regular
// waiters with `wake_success=false`. Used by croutine_channel_close on both the
// send and recv queues.
static void wake_all_as_closed_locked(struct ChannelWaitQueue *queue) {
    struct ChannelWaiter *w;
    while ((w = wait_queue_pop_head(queue)) != NULL) {
        if (w->kind == CW_SELECT) {
            if (w->state && !w->state->done) {
                complete_waiter_locked(w, CROUTINE_SELECT_STATUS_CLOSED);
            }
        } else if (!worker_wake_task_locked(w->task, false)) {
            worker_fail("channel: could not schedule a task woken by close");
        }
    }
}

// -----------------------------------------------------------------------------
// Non-blocking attempt primitives (used by both regular ops and select Phase A)
// -----------------------------------------------------------------------------

// Internal sentinel used only by try_send_locked, distinct from the public
// croutine_channelSendResult values. Callers translate it to a parking
// decision.
#define TRY_SEND_WOULD_BLOCK 2

static int32_t try_send_locked(struct croutine_channel *channel,
                               const void *value) {
    if (channel->closed || worker_shutdown_requested_locked()) {
        return CROUTINE_CHANNEL_SEND_CLOSED;
    }

    if (wake_waiting_receiver_locked(channel, value)) {
        return CROUTINE_CHANNEL_SEND_OK;
    }

    if (channel->capacity > 0 && channel->len < channel->capacity) {
        buffer_push(channel, value);
        return CROUTINE_CHANNEL_SEND_OK;
    }

    return TRY_SEND_WOULD_BLOCK;
}

static int32_t try_recv_locked(struct croutine_channel *channel, void *out) {
    if (channel->len > 0) {
        buffer_pop(channel, out);
        if (!channel->closed) {
            (void)buffer_one_waiting_sender_locked(channel);
        }
        return CROUTINE_CHANNEL_RECV_VALUE;
    }

    if (wake_waiting_sender_direct_locked(channel, out)) {
        return CROUTINE_CHANNEL_RECV_VALUE;
    }

    if (channel->closed || worker_shutdown_requested_locked()) {
        return CROUTINE_CHANNEL_RECV_CLOSED;
    }

    return CROUTINE_CHANNEL_RECV_EMPTY;
}

// -----------------------------------------------------------------------------
// Channel allocation and core API
// -----------------------------------------------------------------------------

struct croutine_channel *croutine_channel_create(size_t elem_size,
                                                 size_t capacity) {
    if (capacity != 0 && elem_size > SIZE_MAX / capacity) {
        return NULL;
    }
    struct croutine_channel *channel = calloc(1, sizeof(*channel));
    if (!channel) {
        return NULL;
    }

    channel->elem_size = elem_size;
    channel->capacity = capacity;
    channel->closed = false;
    channel->len = 0;
    channel->head = 0;
    channel->tail = 0;
    channel->send_waiters.head = NULL;
    channel->send_waiters.tail = NULL;
    channel->recv_waiters.head = NULL;
    channel->recv_waiters.tail = NULL;

    if (capacity == 0) {
        channel->buffer = NULL;
    } else {
        size_t buffer_size = elem_size * capacity;
        if (buffer_size == 0) {
            buffer_size = 1;
        }

        channel->buffer = malloc(buffer_size);
        if (!channel->buffer) {
            free(channel);
            return NULL;
        }
    }

    worker_scheduler_lock();
    channel->registry_next = channel_registry;
    if (channel_registry) {
        channel_registry->registry_prev = channel;
    }
    channel_registry = channel;
    worker_scheduler_unlock();
    return channel;
}

croutine_channel_send_result
croutine_channel_send(struct croutine_channel *channel, const void *value) {
    if (!channel || !value) {
        return CROUTINE_CHANNEL_SEND_CLOSED;
    }

    worker_scheduler_lock();
    const int32_t result = try_send_locked(channel, value);
    if (result == CROUTINE_CHANNEL_SEND_OK) {
        worker_scheduler_unlock();
        return result;
    }

    if (result != TRY_SEND_WOULD_BLOCK) {
        worker_scheduler_unlock();
        return CROUTINE_CHANNEL_SEND_CLOSED;
    }

    struct Task *task = worker_current_task();
    if (!task) {
        worker_scheduler_unlock();
        return CROUTINE_CHANNEL_SEND_CLOSED;
    }

    struct ChannelWaiter waiter = {0};
    waiter.kind = CW_REGULAR;
    waiter.op = CROUTINE_SELECT_OP_SEND;
    waiter.task = task;
    waiter.value_slot = (void *)value;
    wait_queue_push_tail(&channel->send_waiters, &waiter, channel);

    if (!worker_park_current_on_channel_locked()) {
        // park failed (shutdown): waiter may still be linked
        worker_scheduler_lock();
        if (waiter.channel) {
            wait_queue_unlink(&channel->send_waiters, &waiter);
        }
        worker_scheduler_unlock();
        return CROUTINE_CHANNEL_SEND_CLOSED;
    }

    return task->channel_wait.wake_success ? CROUTINE_CHANNEL_SEND_OK
                                           : CROUTINE_CHANNEL_SEND_CLOSED;
}

bool croutine_channel_try_send(struct croutine_channel *channel,
                               const void *value) {
    if (!channel || !value) {
        return 0;
    }

    worker_scheduler_lock();
    const int32_t result = try_send_locked(channel, value);
    worker_scheduler_unlock();
    return result == CROUTINE_CHANNEL_SEND_OK ? 1 : 0;
}

croutine_channel_recv_result
croutine_channel_recv(struct croutine_channel *channel, void *out) {
    if (!channel || !out) {
        return CROUTINE_CHANNEL_RECV_CLOSED;
    }

    worker_scheduler_lock();
    const int32_t result = try_recv_locked(channel, out);
    if (result != CROUTINE_CHANNEL_RECV_EMPTY) {
        worker_scheduler_unlock();
        return result;
    }

    struct Task *task = worker_current_task();
    if (!task) {
        worker_scheduler_unlock();
        return CROUTINE_CHANNEL_RECV_EMPTY;
    }

    struct ChannelWaiter waiter = {0};
    waiter.kind = CW_REGULAR;
    waiter.op = CROUTINE_SELECT_OP_RECV;
    waiter.task = task;
    waiter.value_slot = out;
    wait_queue_push_tail(&channel->recv_waiters, &waiter, channel);

    if (!worker_park_current_on_channel_locked()) {
        worker_scheduler_lock();
        if (waiter.channel) {
            wait_queue_unlink(&channel->recv_waiters, &waiter);
        }
        worker_scheduler_unlock();
        return CROUTINE_CHANNEL_RECV_CLOSED;
    }

    return task->channel_wait.wake_success ? CROUTINE_CHANNEL_RECV_VALUE
                                           : CROUTINE_CHANNEL_RECV_CLOSED;
}

bool croutine_channel_try_recv(struct croutine_channel *channel, void *out) {
    if (!channel || !out) {
        return 0;
    }

    worker_scheduler_lock();
    const int32_t result = try_recv_locked(channel, out);
    worker_scheduler_unlock();
    return result == CROUTINE_CHANNEL_RECV_VALUE ? 1 : 0;
}

static void channel_close_locked(struct croutine_channel *channel) {
    if (channel->closed) {
        return;
    }

    channel->closed = true;
    wake_all_as_closed_locked(&channel->send_waiters);
    wake_all_as_closed_locked(&channel->recv_waiters);
}

void croutine_channel_close(struct croutine_channel *channel) {
    if (!channel) {
        return;
    }
    worker_scheduler_lock();
    channel_close_locked(channel);
    worker_scheduler_unlock();
}

void croutine_channel_shutdown_all_locked(void) {
    for (struct croutine_channel *channel = channel_registry; channel;
         channel = channel->registry_next) {
        channel_close_locked(channel);
    }
}

bool croutine_channel_is_closed(struct croutine_channel *channel) {
    if (!channel) {
        return true;
    }

    worker_scheduler_lock();
    const bool closed = channel->closed;
    worker_scheduler_unlock();
    return closed;
}

bool croutine_channel_destroy(struct croutine_channel *channel) {
    if (!channel) {
        return true;
    }
    worker_scheduler_lock();
    bool safe = channel->closed && !channel->send_waiters.head &&
                !channel->recv_waiters.head;
    if (!safe) {
        worker_scheduler_unlock();
        return false;
    }
    if (channel->registry_prev) {
        channel->registry_prev->registry_next = channel->registry_next;
    } else {
        channel_registry = channel->registry_next;
    }
    if (channel->registry_next) {
        channel->registry_next->registry_prev = channel->registry_prev;
    }
    worker_scheduler_unlock();
    free(channel->buffer);
    free(channel);
    return true;
}

// -----------------------------------------------------------------------------
// select implementation
// -----------------------------------------------------------------------------

// Try to satisfy a single case immediately (must hold scheduler lock).
// Returns true if the case fired; updates the case's `status` field on success.
static bool try_select_case_locked(croutine_select_case *cs) {
    struct croutine_channel *channel = cs->channel;
    if (!channel) {
        return false;
    }

    if (cs->op == CROUTINE_SELECT_OP_SEND) {
        int32_t r = try_send_locked(channel, cs->value);
        if (r == CROUTINE_CHANNEL_SEND_OK) {
            cs->status = CROUTINE_SELECT_STATUS_SENT;
            return true;
        }
        // Closed sends in select fire with CLOSED status (so the caller can
        // observe and panic if appropriate, mirroring Go where send on closed
        // panics even in select).
        if (r == CROUTINE_CHANNEL_SEND_CLOSED) {
            cs->status = CROUTINE_SELECT_STATUS_CLOSED;
            return true;
        }
        return false;
    }

    if (cs->op == CROUTINE_SELECT_OP_RECV) {
        int32_t r = try_recv_locked(channel, cs->value);
        if (r == CROUTINE_CHANNEL_RECV_VALUE) {
            cs->status = CROUTINE_SELECT_STATUS_VALUE;
            return true;
        }
        if (r == CROUTINE_CHANNEL_RECV_CLOSED) {
            cs->status = CROUTINE_SELECT_STATUS_CLOSED;
            return true;
        }
        return false;
    }

    return false;
}

// Fisher-Yates shuffle on an index array using a per-thread xorshift PRNG.
// croutine_select runs concurrently on every worker; using libc rand() would
// race on its internal state. The seed is lazily initialized from the task
// pointer and the wall clock so distinct workers diverge quickly.
static _Thread_local uint64_t select_rng_state = 0;

static uint64_t select_rng_next(void) {
    if (select_rng_state == 0) {
        select_rng_state = (uint64_t)(uintptr_t)worker_current_task() ^
                           ((uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL);
        if (select_rng_state == 0) {
            select_rng_state = 0x9E3779B97F4A7C15ULL;
        }
    }
    uint64_t x = select_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    select_rng_state = x;
    return x;
}

// Remove every select waiter still linked into a channel's queue. Used both
// on park failure (none have fired) and after wakeup (one fired and was
// already unlinked by the waker; we drop the rest).
static void scrub_select_waiters(struct ChannelWaiter *waiters, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        struct ChannelWaiter *w = &waiters[i];
        if (!w->channel) {
            continue;
        }
        struct ChannelWaitQueue *q = (w->op == CROUTINE_SELECT_OP_SEND)
                                         ? &w->channel->send_waiters
                                         : &w->channel->recv_waiters;
        wait_queue_unlink(q, w);
    }
}

static void shuffle_indices(uint32_t *idx, size_t n) {
    for (size_t i = n; i > 1; --i) {
        size_t j = (size_t)(select_rng_next() % (uint64_t)i);
        uint32_t tmp = idx[i - 1];
        idx[i - 1] = idx[j];
        idx[j] = tmp;
    }
}

int croutine_select(croutine_select_case *cases, size_t n, bool has_default) {
    if (n == 0) {
        // `select { default => ... }` is legal and takes the default arm.
        if (has_default) {
            return CROUTINE_SELECT_DEFAULT_INDEX;
        }
        // No cases and no default. The parser rejects `select {}`, so reaching
        // here means a frontend or FFI caller built something it should not
        // have. Parking would block this task forever with nothing able to
        // wake it, so say what happened instead of hanging.
        worker_fail("croutine_select: no cases and no default arm");
    }

    // Shuffling the try order is what keeps one case from starving the others.
    // It needs scratch space: a stack array for the common case, a heap one
    // beyond 32 cases. If that allocation fails, fall back to source order —
    // biased, but every case is still tried, which beats abandoning a select
    // whose cases may well be ready.
    uint32_t order_buf[32];
    uint32_t *order_heap = NULL;
    uint32_t *order = NULL;
    if (n <= sizeof(order_buf) / sizeof(order_buf[0])) {
        order = order_buf;
    } else {
        order_heap = malloc(n * sizeof(uint32_t));
        order = order_heap;
    }
    if (order) {
        for (size_t i = 0; i < n; ++i) {
            order[i] = (uint32_t)i;
        }
        shuffle_indices(order, n);
    }

    worker_scheduler_lock();

    // Phase A: try every case once in randomized order.
    size_t registered_waiters = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t index = order ? order[i] : (uint32_t)i;
        croutine_select_case *cs = &cases[index];
        if (try_select_case_locked(cs)) {
            int32_t chosen = (int32_t)index;
            worker_scheduler_unlock();
            free(order_heap);
            return chosen;
        }
    }

    if (has_default) {
        worker_scheduler_unlock();
        free(order_heap);
        return CROUTINE_SELECT_DEFAULT_INDEX;
    }

    // Phase B: nothing was ready and no default — register waiters and park.
    struct Task *task = worker_current_task();
    if (!task) {
        worker_scheduler_unlock();
        free(order_heap);
        return CROUTINE_SELECT_DEFAULT_INDEX;
    }

    struct CroutineSelectState state = {0};
    state.done = false;
    state.chosen_index = -1;
    state.chosen_status = 0;

    struct ChannelWaiter waiters_buf[32];
    struct ChannelWaiter *waiters = waiters_buf;
    struct ChannelWaiter *waiters_heap = NULL;
    if (n > sizeof(waiters_buf) / sizeof(waiters_buf[0])) {
        waiters_heap = calloc(n, sizeof(struct ChannelWaiter));
        if (!waiters_heap) {
            worker_scheduler_unlock();
            free(order_heap);
            return CROUTINE_SELECT_DEFAULT_INDEX;
        }
        waiters = waiters_heap;
    } else {
        memset(waiters_buf, 0, sizeof(waiters_buf));
    }

    for (size_t i = 0; i < n; ++i) {
        struct ChannelWaiter *w = &waiters[i];
        w->kind = CW_SELECT;
        w->op = (int)cases[i].op;
        w->task = task;
        w->value_slot = cases[i].value;
        w->state = &state;
        w->case_index = (uint32_t)i;
        struct croutine_channel *ch = cases[i].channel;
        if (!ch) {
            continue;
        }
        struct ChannelWaitQueue *q = (cases[i].op == CROUTINE_SELECT_OP_SEND)
                                         ? &ch->send_waiters
                                         : &ch->recv_waiters;
        wait_queue_push_tail(q, w, ch);
        registered_waiters++;
    }

    if (registered_waiters == 0) {
        worker_scheduler_unlock();
        free(waiters_heap);
        free(order_heap);
        return CROUTINE_SELECT_DEFAULT_INDEX;
    }

    if (!worker_park_current_on_channel_locked()) {
        // Park failed (shutdown). Re-acquire lock to scrub residual waiters.
        worker_scheduler_lock();
        scrub_select_waiters(waiters, n);
        worker_scheduler_unlock();
        free(waiters_heap);
        free(order_heap);
        return CROUTINE_SELECT_DEFAULT_INDEX;
    }

    // We woke; remove any leftover waiters that were not the firing case.
    worker_scheduler_lock();
    scrub_select_waiters(waiters, n);
    worker_scheduler_unlock();

    int32_t chosen = state.chosen_index;
    if (chosen >= 0 && (size_t)chosen < n) {
        cases[chosen].status = state.chosen_status;
    }

    free(waiters_heap);
    free(order_heap);
    return chosen;
}
