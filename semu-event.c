#include "semu-event.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#define SEMU_EVENT_VALID_MASK \
    (SEMU_EVENT_READABLE | SEMU_EVENT_WRITABLE | SEMU_EVENT_ERROR)

static bool semu_event_mask_valid(uint32_t events)
{
    return events != 0 && (events & ~SEMU_EVENT_VALID_MASK) == 0;
}

static int semu_event_check_fd(int fd)
{
    if (fd < 0)
        return -EINVAL;
    if (fcntl(fd, F_GETFD) < 0)
        return -errno;
    return 0;
}

static ssize_t semu_event_find_fd(const struct semu_event_loop *loop, int fd)
{
    for (size_t i = 0; i < loop->count; i++) {
        if (loop->fds[i] == fd)
            return (ssize_t) i;
    }
    return -1;
}

static short semu_event_to_poll_events(uint32_t events)
{
    short poll_events = 0;

    if (events & SEMU_EVENT_READABLE)
        poll_events |= POLLIN;
    if (events & SEMU_EVENT_WRITABLE)
        poll_events |= POLLOUT;

    return poll_events;
}

static uint32_t semu_event_from_poll_revents(short revents)
{
    uint32_t events = 0;

    if (revents & (POLLIN | POLLHUP))
        events |= SEMU_EVENT_READABLE;
    if (revents & POLLOUT)
        events |= SEMU_EVENT_WRITABLE;
    if (revents & (POLLERR | POLLNVAL | POLLHUP))
        events |= SEMU_EVENT_ERROR;

    return events;
}


static int semu_event_move_from_stdio(int *fd)
{
    int new_fd;

    if (*fd > STDERR_FILENO)
        return 0;

    new_fd = fcntl(*fd, F_DUPFD, STDERR_FILENO + 1);
    if (new_fd < 0)
        return -errno;

    close(*fd);
    *fd = new_fd;
    return 0;
}

static int semu_event_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -errno;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -errno;
    return 0;
}

int semu_event_loop_init(struct semu_event_loop *loop, const char *name)
{
    if (!loop)
        return -EINVAL;

    memset(loop, 0, sizeof(*loop));
    loop->name = name;
    for (size_t i = 0; i < SEMU_EVENT_LOOP_MAX_FDS; i++)
        loop->fds[i] = -1;
    return 0;
}

int semu_event_add_fd(struct semu_event_loop *loop,
                      int fd,
                      semu_event_token_t token,
                      uint32_t events)
{
    int ret;

    if (!loop)
        return -EINVAL;
    ret = semu_event_check_fd(fd);
    if (ret < 0)
        return ret;
    if (!semu_event_mask_valid(events))
        return -EINVAL;
    if (semu_event_find_fd(loop, fd) >= 0)
        return -EEXIST;
    if (loop->count >= SEMU_EVENT_LOOP_MAX_FDS)
        return -ENOSPC;

    loop->fds[loop->count] = fd;
    loop->tokens[loop->count] = token;
    loop->events[loop->count] = events;
    loop->count++;
    return 0;
}

int semu_event_mod_fd(struct semu_event_loop *loop,
                      int fd,
                      semu_event_token_t token,
                      uint32_t events)
{
    ssize_t index;
    int ret;

    if (!loop)
        return -EINVAL;
    ret = semu_event_check_fd(fd);
    if (ret < 0)
        return ret;
    if (!semu_event_mask_valid(events))
        return -EINVAL;

    index = semu_event_find_fd(loop, fd);
    if (index < 0)
        return -ENOENT;

    loop->tokens[index] = token;
    loop->events[index] = events;
    return 0;
}

int semu_event_del_fd(struct semu_event_loop *loop, int fd)
{
    ssize_t index;
    int ret;

    if (!loop)
        return -EINVAL;
    ret = semu_event_check_fd(fd);
    if (ret < 0)
        return ret;

    index = semu_event_find_fd(loop, fd);
    if (index < 0)
        return -ENOENT;

    size_t last = loop->count - 1;
    if ((size_t) index != last) {
        loop->fds[index] = loop->fds[last];
        loop->tokens[index] = loop->tokens[last];
        loop->events[index] = loop->events[last];
    }
    loop->fds[last] = -1;
    loop->tokens[last] = 0;
    loop->events[last] = 0;
    loop->count--;
    return 0;
}

int semu_event_wait(struct semu_event_loop *loop,
                    struct semu_event *events,
                    size_t cap,
                    int timeout_ms)
{
    struct pollfd pfds[SEMU_EVENT_LOOP_MAX_FDS];
    int ready;
    size_t out = 0;

    if (!loop || !events || cap == 0)
        return -EINVAL;

    for (size_t i = 0; i < loop->count; i++) {
        pfds[i].fd = loop->fds[i];
        pfds[i].events = semu_event_to_poll_events(loop->events[i]);
        pfds[i].revents = 0;
    }

    do {
        ready = poll(pfds, loop->count, timeout_ms);
    } while (ready < 0 && errno == EINTR);

    if (ready < 0)
        return -errno;
    if (ready == 0)
        return 0;

    for (size_t i = 0; i < loop->count && out < cap; i++) {
        uint32_t revents = semu_event_from_poll_revents(pfds[i].revents);

        if (revents == 0)
            continue;
        events[out].token = loop->tokens[i];
        events[out].events = revents;
        out++;
    }

    return (int) out;
}

void semu_event_loop_destroy(struct semu_event_loop *loop)
{
    if (!loop)
        return;

    loop->name = NULL;
    loop->count = 0;
    for (size_t i = 0; i < SEMU_EVENT_LOOP_MAX_FDS; i++) {
        loop->fds[i] = -1;
        loop->tokens[i] = 0;
        loop->events[i] = 0;
    }
}

int semu_event_notifier_init(struct semu_event_notifier *notifier)
{
    int fds[2] = {-1, -1};
    int ret;

    if (!notifier)
        return -EINVAL;

    notifier->read_fd = -1;
    notifier->write_fd = -1;

    if (pipe(fds) < 0)
        return -errno;

    ret = semu_event_move_from_stdio(&fds[0]);
    if (ret == 0)
        ret = semu_event_move_from_stdio(&fds[1]);
    if (ret == 0)
        ret = semu_event_set_nonblock(fds[0]);
    if (ret == 0)
        ret = semu_event_set_nonblock(fds[1]);
    if (ret < 0) {
        close(fds[0]);
        close(fds[1]);
        return ret;
    }

    notifier->read_fd = fds[0];
    notifier->write_fd = fds[1];
    return 0;
}

int semu_event_notifier_signal(struct semu_event_notifier *notifier)
{
    uint8_t byte = 1;

    if (!notifier || notifier->write_fd < 0)
        return -EINVAL;

    for (;;) {
        ssize_t n = write(notifier->write_fd, &byte, sizeof(byte));

        if (n == (ssize_t) sizeof(byte))
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n < 0)
            return -errno;
        return -EIO;
    }
}

int semu_event_notifier_drain(struct semu_event_notifier *notifier)
{
    uint8_t buf[64];

    if (!notifier || notifier->read_fd < 0)
        return -EINVAL;

    for (;;) {
        ssize_t n = read(notifier->read_fd, buf, sizeof(buf));

        if (n > 0)
            continue;
        if (n == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -errno;
    }
}

void semu_event_notifier_destroy(struct semu_event_notifier *notifier)
{
    if (!notifier)
        return;

    if (notifier->read_fd > 0) {
        close(notifier->read_fd);
        notifier->read_fd = -1;
    } else {
        notifier->read_fd = -1;
    }

    if (notifier->write_fd > 0) {
        close(notifier->write_fd);
        notifier->write_fd = -1;
    } else {
        notifier->write_fd = -1;
    }
}
