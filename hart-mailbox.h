#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct semu_hart_mailbox {
    _Atomic uint32_t pending_events;
    _Atomic uint32_t ack_generation;
    _Atomic uint64_t pause_request_seq;
    _Atomic uint64_t pause_ack_seq;
} semu_hart_mailbox_t;

enum semu_hart_mailbox_event {
    SEMU_HART_MAILBOX_RFENCE = UINT32_C(1) << 0,
    SEMU_HART_MAILBOX_HSM_RESUME = UINT32_C(1) << 1,
    SEMU_HART_MAILBOX_PAUSE = UINT32_C(1) << 2,
};

#define SEMU_HART_MAILBOX_ALL_EVENTS                           \
    (SEMU_HART_MAILBOX_RFENCE | SEMU_HART_MAILBOX_HSM_RESUME | \
     SEMU_HART_MAILBOX_PAUSE)

static inline bool semu_hart_mailbox_event_mask_valid(uint32_t events)
{
    return events != 0 && (events & ~SEMU_HART_MAILBOX_ALL_EVENTS) == 0;
}

static inline void semu_hart_mailbox_init(semu_hart_mailbox_t *mailbox)
{
    __atomic_store_n(&mailbox->pending_events, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&mailbox->ack_generation, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&mailbox->pause_request_seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&mailbox->pause_ack_seq, 0, __ATOMIC_RELAXED);
}

static inline uint32_t semu_hart_mailbox_pending_load(
    const semu_hart_mailbox_t *mailbox)
{
    return __atomic_load_n(&mailbox->pending_events, __ATOMIC_ACQUIRE);
}

static inline bool semu_hart_mailbox_has(const semu_hart_mailbox_t *mailbox,
                                         uint32_t events)
{
    if (!semu_hart_mailbox_event_mask_valid(events))
        return false;
    return (semu_hart_mailbox_pending_load(mailbox) & events) == events;
}

static inline void semu_hart_mailbox_request(semu_hart_mailbox_t *mailbox,
                                             uint32_t events)
{
    if (!semu_hart_mailbox_event_mask_valid(events))
        return;
    __atomic_fetch_or(&mailbox->pending_events, events, __ATOMIC_RELEASE);
}

static inline void semu_hart_mailbox_clear(semu_hart_mailbox_t *mailbox,
                                           uint32_t events)
{
    if (!semu_hart_mailbox_event_mask_valid(events))
        return;
    __atomic_fetch_and(&mailbox->pending_events, ~events, __ATOMIC_RELEASE);
}

static inline uint32_t semu_hart_mailbox_ack(semu_hart_mailbox_t *mailbox)
{
    return __atomic_add_fetch(&mailbox->ack_generation, 1, __ATOMIC_ACQ_REL);
}

static inline uint32_t semu_hart_mailbox_ack_generation(
    const semu_hart_mailbox_t *mailbox)
{
    return __atomic_load_n(&mailbox->ack_generation, __ATOMIC_ACQUIRE);
}


static inline uint64_t semu_hart_mailbox_pause_request_seq(
    const semu_hart_mailbox_t *mailbox)
{
    return __atomic_load_n(&mailbox->pause_request_seq, __ATOMIC_ACQUIRE);
}

static inline uint64_t semu_hart_mailbox_pause_ack_seq(
    const semu_hart_mailbox_t *mailbox)
{
    return __atomic_load_n(&mailbox->pause_ack_seq, __ATOMIC_ACQUIRE);
}

static inline void semu_hart_mailbox_pause_request(semu_hart_mailbox_t *mailbox,
                                                   uint64_t seq)
{
    __atomic_store_n(&mailbox->pause_request_seq, seq, __ATOMIC_RELEASE);
    semu_hart_mailbox_request(mailbox, SEMU_HART_MAILBOX_PAUSE);
}

static inline void semu_hart_mailbox_pause_ack(semu_hart_mailbox_t *mailbox,
                                               uint64_t seq)
{
    __atomic_store_n(&mailbox->pause_ack_seq, seq, __ATOMIC_RELEASE);
    semu_hart_mailbox_clear(mailbox, SEMU_HART_MAILBOX_PAUSE);
}
