#ifndef SEMU_EVENT_H
#define SEMU_EVENT_H

#include <stddef.h>
#include <stdint.h>

#define SEMU_EVENT_LOOP_MAX_FDS 64U

enum semu_event_kind {
    SEMU_EVENT_READABLE = 1u << 0,
    SEMU_EVENT_WRITABLE = 1u << 1,
    SEMU_EVENT_ERROR = 1u << 2,
};

typedef uint32_t semu_event_token_t;

enum semu_event_reserved_token {
    SEMU_EVENT_TOKEN_PAUSE = 0,
    SEMU_EVENT_TOKEN_KILL = 1,
    SEMU_EVENT_TOKEN_RESET = 2,
    SEMU_EVENT_TOKEN_QUEUE_PENDING = 3,
    SEMU_EVENT_TOKEN_FIRST_DEVICE = 16,
};

struct semu_event {
    semu_event_token_t token;
    uint32_t events;
};

/* Fixed-capacity poll() backend. Registration and removal are serialized by
 * the loop owner; this slice intentionally provides no loop-internal lock.
 */
struct semu_event_loop {
    const char *name;
    size_t count;
    int fds[SEMU_EVENT_LOOP_MAX_FDS];
    semu_event_token_t tokens[SEMU_EVENT_LOOP_MAX_FDS];
    uint32_t events[SEMU_EVENT_LOOP_MAX_FDS];
};

int semu_event_loop_init(struct semu_event_loop *loop, const char *name);
int semu_event_add_fd(struct semu_event_loop *loop,
                      int fd,
                      semu_event_token_t token,
                      uint32_t events);
int semu_event_mod_fd(struct semu_event_loop *loop,
                      int fd,
                      semu_event_token_t token,
                      uint32_t events);
int semu_event_del_fd(struct semu_event_loop *loop, int fd);
int semu_event_wait(struct semu_event_loop *loop,
                    struct semu_event *events,
                    size_t cap,
                    int timeout_ms);
void semu_event_loop_destroy(struct semu_event_loop *loop);

struct semu_event_notifier {
    int read_fd;
    int write_fd;
};

int semu_event_notifier_init(struct semu_event_notifier *notifier);
int semu_event_notifier_signal(struct semu_event_notifier *notifier);
int semu_event_notifier_drain(struct semu_event_notifier *notifier);
void semu_event_notifier_destroy(struct semu_event_notifier *notifier);

#endif
