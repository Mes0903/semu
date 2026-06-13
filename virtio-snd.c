#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <portaudio.h>

#include "common.h"
#include "device.h"
#include "ram_access.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio-irq.h"
#include "virtio-mmio.h"
#include "virtio.h"
#include "virtq.h"

#define VSND_DEV_CNT_MAX 1
#define VSND_QUEUE_NUM_MAX 1024
#define VSND_CNFA_FRAME_SZ 2
#define VIRTIO_SND_F_VERSION_1 (UINT64_C(1) << 32)

#define PRIV(vsnd) ((virtio_snd_config_t *) (vsnd)->priv)

enum {
    VSND_QUEUE_CTRL = 0,
    VSND_QUEUE_EVT = 1,
    VSND_QUEUE_TX = 2,
    VSND_QUEUE_RX = 3,
    VSND_QUEUE_COUNT = 4,
};

enum {
    VIRTIO_SND_R_JACK_INFO = 1,
    VIRTIO_SND_R_PCM_INFO = 0x0100,
    VIRTIO_SND_R_PCM_SET_PARAMS,
    VIRTIO_SND_R_PCM_PREPARE,
    VIRTIO_SND_R_PCM_RELEASE,
    VIRTIO_SND_R_PCM_START,
    VIRTIO_SND_R_PCM_STOP,
    VIRTIO_SND_R_CHMAP_INFO = 0x0200,
    VIRTIO_SND_S_OK = 0x8000,
    VIRTIO_SND_S_BAD_MSG,
    VIRTIO_SND_S_NOT_SUPP,
    VIRTIO_SND_S_IO_ERR,
};

#define SND_PCM_RATE \
    _(5512)          \
    _(8000)          \
    _(11025)         \
    _(16000)         \
    _(22050)         \
    _(32000)         \
    _(44100)         \
    _(48000)         \
    _(64000)         \
    _(88200)         \
    _(96000)         \
    _(176400)        \
    _(192000)        \
    _(384000)

enum {
#define _(rate) VIRTIO_SND_PCM_RATE_##rate,
    SND_PCM_RATE
#undef _
};

static const int pcm_rate_tbl[] = {
#define _(rate) [VIRTIO_SND_PCM_RATE_##rate] = rate,
    SND_PCM_RATE
#undef _
};

enum {
    VIRTIO_SND_PCM_FMT_S16 = 5,
};

enum {
    VIRTIO_SND_CHMAP_MONO = 2,
};

enum {
    VIRTIO_SND_D_OUTPUT = 0,
    VIRTIO_SND_D_INPUT,
};

typedef struct {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
    uint32_t controls;
} virtio_snd_config_t;

typedef struct {
    uint32_t code;
} virtio_snd_hdr_t;

typedef struct {
    uint32_t hda_fn_nid;
} virtio_snd_info_t;

typedef struct {
    virtio_snd_hdr_t hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
} virtio_snd_query_info_t;

typedef struct {
    virtio_snd_info_t hdr;
    uint32_t features;
    uint32_t hda_reg_defconf;
    uint32_t hda_reg_caps;
    uint8_t connected;
    uint8_t padding[7];
} virtio_snd_jack_info_t;

typedef struct {
    virtio_snd_info_t hdr;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
} virtio_snd_pcm_info_t;

typedef struct {
    virtio_snd_hdr_t hdr;
    uint32_t stream_id;
} virtio_snd_pcm_hdr_t;

typedef struct {
    virtio_snd_pcm_hdr_t hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
} virtio_snd_pcm_set_params_t;

typedef struct {
    uint32_t stream_id;
} virtio_snd_pcm_xfer_t;

typedef struct {
    uint32_t status;
    uint32_t latency_bytes;
} virtio_snd_pcm_status_t;

#define VIRTIO_SND_CHMAP_MAX_SIZE 18

typedef struct {
    virtio_snd_info_t hdr;
    uint8_t direction;
    uint8_t channels;
    uint8_t positions[VIRTIO_SND_CHMAP_MAX_SIZE];
} virtio_snd_chmap_info_t;

typedef struct {
    pthread_cond_t readable, writable;
    int buf_ev_notify;
    int releasing;
    pthread_mutex_t lock;
} virtio_snd_queue_lock_t;

typedef struct {
    uint32_t stream_id;
} vsnd_stream_sel_t;

typedef struct {
    void *addr;
    uint32_t len;
    uint32_t pos;
    struct list_head q;
} vsnd_buf_queue_node_t;

typedef struct {
    virtio_snd_jack_info_t j;
    virtio_snd_pcm_info_t p;
    virtio_snd_chmap_info_t c;
    virtio_snd_pcm_set_params_t pp;
    PaStream *pa_stream;
    virtio_snd_queue_lock_t lock;
    struct list_head buf_queue_head;
    vsnd_stream_sel_t v;
} virtio_snd_prop_t;

static virtio_snd_config_t vsnd_configs[VSND_DEV_CNT_MAX];
static virtio_snd_prop_t vsnd_props[VSND_DEV_CNT_MAX] = {
    [0 ... VSND_DEV_CNT_MAX - 1].pp.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS,
    [0 ... VSND_DEV_CNT_MAX - 1].lock =
        {
            .lock = PTHREAD_MUTEX_INITIALIZER,
            .readable = PTHREAD_COND_INITIALIZER,
            .writable = PTHREAD_COND_INITIALIZER,
        },
};
static int vsnd_dev_cnt;

static int virtio_snd_stream_cb(const void *input,
                                void *output,
                                unsigned long frame_cnt,
                                const PaStreamCallbackTimeInfo *time_info,
                                PaStreamCallbackFlags status_flags,
                                void *user_data);

static inline unsigned virtio_snd_status_load(virtio_snd_state_t *vsnd)
{
    return atomic_load_explicit(&vsnd->common.status, memory_order_acquire);
}

static void virtio_snd_set_fail(virtio_snd_state_t *vsnd)
{
    unsigned status = virtio_snd_status_load(vsnd);

    virtio_device_common_set_needs_reset(&vsnd->common);
    if (status & VIRTIO_STATUS__DRIVER_OK)
        virtio_irq_trigger(&vsnd->common.irq, VIRTIO_INT__CONF_CHANGE);
}

static bool virtio_snd_config_range_valid(uint32_t offset, uint32_t size)
{
    return size != 0 && offset < sizeof(virtio_snd_config_t) &&
           size <= sizeof(virtio_snd_config_t) - offset;
}

static uint32_t virtio_snd_read_config(void *opaque,
                                       uint32_t offset,
                                       uint32_t size)
{
    virtio_snd_state_t *vsnd = opaque;
    uint32_t value = 0;

    if (!vsnd || !vsnd->priv || !virtio_snd_config_range_valid(offset, size))
        return 0;

    memcpy(&value, (uint8_t *) PRIV(vsnd) + offset, size);
    return value;
}

static void virtio_snd_write_config(void *opaque,
                                    uint32_t offset,
                                    uint32_t size,
                                    uint32_t value)
{
    virtio_snd_state_t *vsnd = opaque;

    if (!vsnd || !vsnd->priv || !virtio_snd_config_range_valid(offset, size))
        return;

    memcpy((uint8_t *) PRIV(vsnd) + offset, &value, size);
}

static void virtio_snd_init_stream_defaults(virtio_snd_prop_t *props)
{
    memset(&props->j, 0, sizeof(props->j));
    memset(&props->p, 0, sizeof(props->p));
    memset(&props->c, 0, sizeof(props->c));
    memset(&props->pp, 0, sizeof(props->pp));
    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    props->pa_stream = NULL;
    props->v.stream_id = 0;
    INIT_LIST_HEAD(&props->buf_queue_head);
    props->lock.buf_ev_notify = 0;
    props->lock.releasing = 0;
}

static void virtio_snd_free_buffer_queue_locked(virtio_snd_prop_t *props)
{
    vsnd_buf_queue_node_t *node;
    vsnd_buf_queue_node_t *tmp;

    if (!props)
        return;

    list_for_each_entry_safe (node, tmp, &props->buf_queue_head, q) {
        list_del(&node->q);
        free(node->addr);
        free(node);
    }
    props->lock.buf_ev_notify = 0;
    pthread_cond_broadcast(&props->lock.writable);
}

static void virtio_snd_mark_stream_releasing(virtio_snd_prop_t *props)
{
    pthread_mutex_lock(&props->lock.lock);
    props->lock.releasing = 1;
    pthread_cond_broadcast(&props->lock.readable);
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);
}

static void virtio_snd_mark_all_streams_releasing(void)
{
    for (uint32_t i = 0; i < VSND_DEV_CNT_MAX; i++)
        virtio_snd_mark_stream_releasing(&vsnd_props[i]);
}

static void virtio_snd_close_stream_callbacks_first(virtio_snd_prop_t *props)
{
    PaStream *stream;
    uint32_t code;

    if (!props)
        return;

    virtio_snd_mark_stream_releasing(props);

    stream = props->pa_stream;
    code = props->pp.hdr.hdr.code;
    if (stream) {
        if (code == VIRTIO_SND_R_PCM_START) {
            PaError stop_err = Pa_StopStream(stream);

            if (stop_err != paNoError)
                fprintf(stderr, "virtio-snd: Pa_StopStream: %s\n",
                        Pa_GetErrorText(stop_err));
        }

        PaError close_err = Pa_CloseStream(stream);
        if (close_err != paNoError)
            fprintf(stderr, "virtio-snd: Pa_CloseStream: %s\n",
                    Pa_GetErrorText(close_err));
        props->pa_stream = NULL;
    }

    pthread_mutex_lock(&props->lock.lock);
    virtio_snd_free_buffer_queue_locked(props);
    pthread_mutex_unlock(&props->lock.lock);
}

static void virtio_snd_stop_all_streams(virtio_snd_state_t *vsnd)
{
    (void) vsnd;

    for (uint32_t i = 0; i < VSND_DEV_CNT_MAX; i++)
        virtio_snd_close_stream_callbacks_first(&vsnd_props[i]);
}

static void virtio_snd_reset_props(void)
{
    for (uint32_t i = 0; i < VSND_DEV_CNT_MAX; i++)
        virtio_snd_init_stream_defaults(&vsnd_props[i]);
}

static bool virtio_snd_query_valid(const virtio_snd_query_info_t *query,
                                   uint32_t available)
{
    if (!query || query->count == 0)
        return false;
    if (query->start_id >= available)
        return false;
    return query->count <= available - query->start_id;
}

static uint64_t virtio_snd_supported_rates(void)
{
    uint64_t rates = 0;

#define _(rate) rates |= UINT64_C(1) << VIRTIO_SND_PCM_RATE_##rate;
    SND_PCM_RATE
#undef _
    return rates;
}

static void virtio_snd_build_jack_info(virtio_snd_jack_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->connected = 1;
}

static void virtio_snd_build_pcm_info(virtio_snd_pcm_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->formats = UINT64_C(1) << VIRTIO_SND_PCM_FMT_S16;
    info->rates = virtio_snd_supported_rates();
    info->direction = VIRTIO_SND_D_OUTPUT;
    info->channels_min = 1;
    info->channels_max = 1;
}

static void virtio_snd_build_chmap_info(virtio_snd_chmap_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->direction = VIRTIO_SND_D_OUTPUT;
    info->channels = 1;
    info->positions[0] = VIRTIO_SND_CHMAP_MONO;
}

static int virtio_snd_iovs_read(virtio_snd_state_t *vsnd,
                                const struct virtq_iov *iov,
                                size_t count,
                                guest_size_t skip,
                                void *dst,
                                guest_size_t len)
{
    uint8_t *out = dst;

    for (size_t i = 0; i < count && len > 0; i++) {
        guest_paddr_t addr = iov[i].addr;
        guest_size_t iov_len = iov[i].len;
        guest_size_t n;

        if (skip >= iov_len) {
            skip -= iov_len;
            continue;
        }

        addr += skip;
        iov_len -= skip;
        skip = 0;
        n = MIN(iov_len, len);
        if (!ram_dma_read(vsnd->common.dma, addr, out, n))
            return -EFAULT;
        out += n;
        len -= n;
    }
    return len == 0 ? 0 : -EINVAL;
}

static int virtio_snd_iovs_write(virtio_snd_state_t *vsnd,
                                 const struct virtq_iov *iov,
                                 size_t count,
                                 guest_size_t skip,
                                 const void *src,
                                 guest_size_t len)
{
    const uint8_t *in = src;

    for (size_t i = 0; i < count && len > 0; i++) {
        guest_paddr_t addr = iov[i].addr;
        guest_size_t iov_len = iov[i].len;
        guest_size_t n;

        if (skip >= iov_len) {
            skip -= iov_len;
            continue;
        }

        addr += skip;
        iov_len -= skip;
        skip = 0;
        n = MIN(iov_len, len);
        if (!ram_dma_write(vsnd->common.dma, addr, in, n))
            return -EFAULT;
        in += n;
        len -= n;
    }
    return len == 0 ? 0 : -EINVAL;
}

static int virtio_snd_write_control_status(virtio_snd_state_t *vsnd,
                                           const struct virtq_chain *chain,
                                           uint32_t status)
{
    virtio_snd_hdr_t response = {
        .code = status,
    };

    if (chain->writable_count == 0)
        return -EINVAL;
    return virtio_snd_iovs_write(vsnd, &chain->writable[0], 1, 0, &response,
                                 sizeof(response));
}

static int virtio_snd_write_control_payload(virtio_snd_state_t *vsnd,
                                            const struct virtq_chain *chain,
                                            const void *payload,
                                            guest_size_t len)
{
    if (len == 0)
        return 0;
    if (chain->writable_count < 2)
        return -EINVAL;
    return virtio_snd_iovs_write(vsnd, &chain->writable[1],
                                 chain->writable_count - 1, 0, payload, len);
}

static int virtio_snd_write_pcm_status(virtio_snd_state_t *vsnd,
                                       const struct virtq_chain *chain,
                                       uint32_t status,
                                       uint32_t latency_bytes)
{
    virtio_snd_pcm_status_t response = {
        .status = status,
        .latency_bytes = latency_bytes,
    };

    if (chain->writable_count == 0)
        return -EINVAL;
    return virtio_snd_iovs_write(vsnd, &chain->writable[0], 1, 0, &response,
                                 sizeof(response));
}

static int virtio_snd_enqueue_guest_iov(virtio_snd_state_t *vsnd,
                                        virtio_snd_prop_t *props,
                                        const struct virtq_iov *iov)
{
    vsnd_buf_queue_node_t *node;

    if (iov->len == 0)
        return 0;

    node = calloc(1, sizeof(*node));
    if (!node)
        return -ENOMEM;
    node->addr = malloc(iov->len);
    if (!node->addr) {
        free(node);
        return -ENOMEM;
    }
    node->len = iov->len;
    node->pos = 0;

    if (!ram_dma_read(vsnd->common.dma, iov->addr, node->addr, iov->len)) {
        free(node->addr);
        free(node);
        return -EFAULT;
    }

    pthread_mutex_lock(&props->lock.lock);
    while (props->lock.buf_ev_notify > 0 && !props->lock.releasing)
        pthread_cond_wait(&props->lock.writable, &props->lock.lock);

    if (props->lock.releasing) {
        pthread_mutex_unlock(&props->lock.lock);
        free(node->addr);
        free(node);
        return 0;
    }

    list_push(&node->q, &props->buf_queue_head);
    props->lock.buf_ev_notify++;
    pthread_cond_signal(&props->lock.readable);
    pthread_mutex_unlock(&props->lock.lock);
    return 0;
}

static void virtio_snd_frame_dequeue(void *out, uint32_t n, uint32_t stream_id)
{
    virtio_snd_prop_t *props;
    uint8_t *bytes = out;
    uint32_t written_bytes = 0;

    if (stream_id >= VSND_DEV_CNT_MAX) {
        memset(out, 0, n);
        return;
    }
    props = &vsnd_props[stream_id];

    pthread_mutex_lock(&props->lock.lock);
    while (props->lock.buf_ev_notify < 1 && !props->lock.releasing)
        pthread_cond_wait(&props->lock.readable, &props->lock.lock);

    if (props->lock.releasing) {
        pthread_mutex_unlock(&props->lock.lock);
        memset(out, 0, n);
        return;
    }

    while (!list_empty(&props->buf_queue_head) && written_bytes < n) {
        vsnd_buf_queue_node_t *node =
            list_first_entry(&props->buf_queue_head, vsnd_buf_queue_node_t, q);
        uint32_t left = n - written_bytes;
        uint32_t actual = node->len - node->pos;
        uint32_t len = MIN(left, actual);

        memcpy(bytes + written_bytes, (uint8_t *) node->addr + node->pos, len);
        written_bytes += len;
        node->pos += len;
        if (node->pos >= node->len) {
            list_del(&node->q);
            free(node->addr);
            free(node);
        }
    }

    if (written_bytes < n)
        memset(bytes + written_bytes, 0, n - written_bytes);
    if (props->lock.buf_ev_notify > 0)
        props->lock.buf_ev_notify--;
    pthread_cond_signal(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);
}

static int virtio_snd_stream_cb(const void *input,
                                void *output,
                                unsigned long frame_cnt,
                                const PaStreamCallbackTimeInfo *time_info,
                                PaStreamCallbackFlags status_flags,
                                void *user_data)
{
    vsnd_stream_sel_t *v_ptr = user_data;
    uint32_t id = v_ptr ? v_ptr->stream_id : 0;
    uint32_t channels = id < VSND_DEV_CNT_MAX ? vsnd_props[id].pp.channels : 1;
    uint32_t out_buf_bytes;

    (void) input;
    (void) time_info;
    (void) status_flags;

    if (channels == 0)
        channels = 1;
    out_buf_bytes = (uint32_t) frame_cnt * channels * VSND_CNFA_FRAME_SZ;
    virtio_snd_frame_dequeue(output, out_buf_bytes, id);
    return paContinue;
}

static int virtio_snd_prepare_stream(virtio_snd_prop_t *props,
                                     const virtio_snd_pcm_hdr_t *hdr)
{
    uint32_t stream_id = hdr->stream_id;
    uint32_t channels = props->pp.channels ? props->pp.channels : 1;
    uint8_t rate_index = props->pp.rate;
    uint32_t rate;
    uint32_t bps_rate;
    uint32_t period_bytes;
    uint32_t period_frames;
    PaStreamParameters params;
    PaError err;

    if (stream_id >= VSND_DEV_CNT_MAX || rate_index >= ARRAY_SIZE(pcm_rate_tbl))
        return -EINVAL;
    rate = (uint32_t) pcm_rate_tbl[rate_index];
    if (rate == 0)
        rate = 48000;

    virtio_snd_close_stream_callbacks_first(props);

    pthread_mutex_lock(&props->lock.lock);
    INIT_LIST_HEAD(&props->buf_queue_head);
    props->lock.releasing = 0;
    props->lock.buf_ev_notify = 0;
    pthread_mutex_unlock(&props->lock.lock);

    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_PREPARE;
    props->v.stream_id = stream_id;

    bps_rate = channels * VSND_CNFA_FRAME_SZ * rate;
    period_bytes = bps_rate / 10;
    period_frames = period_bytes / VSND_CNFA_FRAME_SZ;
    if (period_frames == 0)
        period_frames = 1;

    params = (PaStreamParameters) {
        .device = Pa_GetDefaultOutputDevice(),
        .channelCount = (int) channels,
        .sampleFormat = paInt16,
        .suggestedLatency = 0.1,
        .hostApiSpecificStreamInfo = NULL,
    };

    err = Pa_OpenStream(&props->pa_stream, NULL, &params, rate, period_frames,
                        paClipOff, virtio_snd_stream_cb, &props->v);
    if (err != paNoError) {
        fprintf(stderr, "virtio-snd: Pa_OpenStream: %s\n",
                Pa_GetErrorText(err));
        props->pa_stream = NULL;
        return -EIO;
    }
    return 0;
}

static int virtio_snd_process_control_chain(virtio_snd_state_t *vsnd,
                                            const struct virtq_chain *chain,
                                            uint32_t *used_len,
                                            bool *flush_tx)
{
    virtio_snd_hdr_t hdr;
    uint32_t status = VIRTIO_SND_S_OK;
    uint32_t payload_len = 0;
    uint8_t payload[sizeof(virtio_snd_pcm_info_t) * VSND_DEV_CNT_MAX];
    int ret;

    *used_len = 0;
    *flush_tx = false;
    memset(payload, 0, sizeof(payload));

    if (chain->readable_count == 0 || chain->writable_count == 0)
        return -EINVAL;
    ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count, 0,
                               &hdr, sizeof(hdr));
    if (ret < 0)
        return ret;

    switch (hdr.code) {
    case VIRTIO_SND_R_JACK_INFO: {
        virtio_snd_query_info_t query;

        ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count,
                                   0, &query, sizeof(query));
        if (ret < 0)
            return ret;
        if (!virtio_snd_query_valid(&query, PRIV(vsnd)->jacks)) {
            status = VIRTIO_SND_S_BAD_MSG;
            break;
        }
        for (uint32_t i = 0; i < query.count; i++)
            virtio_snd_build_jack_info(
                &((virtio_snd_jack_info_t *) payload)[i]);
        payload_len = query.count * sizeof(virtio_snd_jack_info_t);
        break;
    }
    case VIRTIO_SND_R_PCM_INFO: {
        virtio_snd_query_info_t query;

        ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count,
                                   0, &query, sizeof(query));
        if (ret < 0)
            return ret;
        if (!virtio_snd_query_valid(&query, PRIV(vsnd)->streams)) {
            status = VIRTIO_SND_S_BAD_MSG;
            break;
        }
        for (uint32_t i = 0; i < query.count; i++)
            virtio_snd_build_pcm_info(&((virtio_snd_pcm_info_t *) payload)[i]);
        payload_len = query.count * sizeof(virtio_snd_pcm_info_t);
        break;
    }
    case VIRTIO_SND_R_CHMAP_INFO: {
        virtio_snd_query_info_t query;

        ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count,
                                   0, &query, sizeof(query));
        if (ret < 0)
            return ret;
        if (!virtio_snd_query_valid(&query, PRIV(vsnd)->chmaps)) {
            status = VIRTIO_SND_S_BAD_MSG;
            break;
        }
        for (uint32_t i = 0; i < query.count; i++)
            virtio_snd_build_chmap_info(
                &((virtio_snd_chmap_info_t *) payload)[i]);
        payload_len = query.count * sizeof(virtio_snd_chmap_info_t);
        break;
    }
    case VIRTIO_SND_R_PCM_SET_PARAMS: {
        virtio_snd_pcm_set_params_t request;
        uint32_t stream_id;

        ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count,
                                   0, &request, sizeof(request));
        if (ret < 0)
            return ret;
        stream_id = request.hdr.stream_id;
        if (stream_id >= VSND_DEV_CNT_MAX) {
            status = VIRTIO_SND_S_BAD_MSG;
            break;
        }
        vsnd_props[stream_id].pp = request;
        vsnd_props[stream_id].pp.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
        break;
    }
    case VIRTIO_SND_R_PCM_PREPARE: {
        virtio_snd_pcm_hdr_t request;

        ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count,
                                   0, &request, sizeof(request));
        if (ret < 0)
            return ret;
        if (request.stream_id >= VSND_DEV_CNT_MAX ||
            virtio_snd_prepare_stream(&vsnd_props[request.stream_id],
                                      &request) < 0)
            status = VIRTIO_SND_S_IO_ERR;
        break;
    }
    case VIRTIO_SND_R_PCM_START: {
        virtio_snd_pcm_hdr_t request;
        virtio_snd_prop_t *props;
        PaError err;

        ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count,
                                   0, &request, sizeof(request));
        if (ret < 0)
            return ret;
        if (request.stream_id >= VSND_DEV_CNT_MAX) {
            status = VIRTIO_SND_S_BAD_MSG;
            break;
        }
        props = &vsnd_props[request.stream_id];
        if (!props->pa_stream) {
            status = VIRTIO_SND_S_IO_ERR;
            break;
        }
        err = Pa_StartStream(props->pa_stream);
        if (err != paNoError) {
            fprintf(stderr, "virtio-snd: Pa_StartStream: %s\n",
                    Pa_GetErrorText(err));
            status = VIRTIO_SND_S_IO_ERR;
        } else {
            props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_START;
        }
        break;
    }
    case VIRTIO_SND_R_PCM_STOP: {
        virtio_snd_pcm_hdr_t request;
        virtio_snd_prop_t *props;
        PaError err;

        ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count,
                                   0, &request, sizeof(request));
        if (ret < 0)
            return ret;
        if (request.stream_id >= VSND_DEV_CNT_MAX) {
            status = VIRTIO_SND_S_BAD_MSG;
            break;
        }
        props = &vsnd_props[request.stream_id];
        if (!props->pa_stream) {
            status = VIRTIO_SND_S_IO_ERR;
            break;
        }
        err = Pa_StopStream(props->pa_stream);
        if (err != paNoError) {
            fprintf(stderr, "virtio-snd: Pa_StopStream: %s\n",
                    Pa_GetErrorText(err));
            status = VIRTIO_SND_S_IO_ERR;
        } else {
            props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_STOP;
        }
        break;
    }
    case VIRTIO_SND_R_PCM_RELEASE: {
        virtio_snd_pcm_hdr_t request;

        ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count,
                                   0, &request, sizeof(request));
        if (ret < 0)
            return ret;
        if (request.stream_id >= VSND_DEV_CNT_MAX) {
            status = VIRTIO_SND_S_BAD_MSG;
            break;
        }
        vsnd_props[request.stream_id].pp.hdr.hdr.code =
            VIRTIO_SND_R_PCM_RELEASE;
        virtio_snd_close_stream_callbacks_first(&vsnd_props[request.stream_id]);
        *flush_tx = true;
        break;
    }
    default:
        status = VIRTIO_SND_S_NOT_SUPP;
        break;
    }

    ret = virtio_snd_write_control_status(vsnd, chain, status);
    if (ret < 0)
        return ret;
    if (status == VIRTIO_SND_S_OK && payload_len > 0) {
        ret =
            virtio_snd_write_control_payload(vsnd, chain, payload, payload_len);
        if (ret < 0)
            return ret;
    }

    *used_len = sizeof(virtio_snd_hdr_t) + payload_len;
    return 0;
}

static int virtio_snd_process_tx_chain(virtio_snd_state_t *vsnd,
                                       const struct virtq_chain *chain,
                                       bool flush,
                                       uint32_t *used_len)
{
    virtio_snd_pcm_xfer_t request;
    uint32_t status = VIRTIO_SND_S_OK;
    uint32_t latency = 0;
    int ret;

    *used_len = 0;
    if (chain->readable_count == 0 || chain->writable_count == 0)
        return -EINVAL;

    ret = virtio_snd_iovs_read(vsnd, chain->readable, chain->readable_count, 0,
                               &request, sizeof(request));
    if (ret < 0)
        return ret;

    if (request.stream_id >= VSND_DEV_CNT_MAX) {
        status = VIRTIO_SND_S_IO_ERR;
    } else {
        virtio_snd_prop_t *props = &vsnd_props[request.stream_id];

        for (size_t i = 1; i < chain->readable_count; i++) {
            if (chain->readable[i].len > UINT32_MAX - latency) {
                status = VIRTIO_SND_S_IO_ERR;
                break;
            }
            latency += chain->readable[i].len;
            if (!flush) {
                ret = virtio_snd_enqueue_guest_iov(vsnd, props,
                                                   &chain->readable[i]);
                if (ret < 0)
                    return ret;
            }
        }
    }

    ret = virtio_snd_write_pcm_status(vsnd, chain, status, latency);
    if (ret < 0)
        return ret;

    *used_len = sizeof(virtio_snd_pcm_status_t);
    return 0;
}

static int virtio_snd_process_rx_chain(virtio_snd_state_t *vsnd,
                                       const struct virtq_chain *chain,
                                       uint32_t *used_len)
{
    int ret;

    *used_len = 0;
    ret = virtio_snd_write_pcm_status(vsnd, chain, VIRTIO_SND_S_IO_ERR, 0);
    if (ret < 0)
        return ret;
    *used_len = sizeof(virtio_snd_pcm_status_t);
    return 0;
}

static int virtio_snd_queue_available(virtio_snd_state_t *vsnd,
                                      struct virtq *queue,
                                      uint16_t *available)
{
    uint16_t avail_idx;
    uint16_t delta;

    if (!vsnd || !queue || !queue->ready || !available)
        return -EINVAL;
    if (!ram_dma_read(vsnd->common.dma, queue->driver_addr + 2, &avail_idx,
                      sizeof(avail_idx)))
        return -EFAULT;

    delta = (uint16_t) (avail_idx - queue->last_avail);
    if (delta > queue->queue_size)
        return -EINVAL;

    *available = delta;
    return 0;
}

static bool virtio_snd_actor_generation_current(virtio_snd_state_t *vsnd,
                                                struct virtio_actor *actor,
                                                uint64_t generation)
{
    (void) vsnd;
    return actor && virtio_actor_generation(actor) == generation;
}

static bool virtio_snd_common_generation_current(virtio_snd_state_t *vsnd,
                                                 uint64_t generation)
{
    bool current;

    if (!vsnd || !vsnd->common.initialized)
        return false;

    pthread_mutex_lock(&vsnd->common.transport_lock);
    current = vsnd->common.generation == generation &&
              !vsnd->common.reset_in_progress;
    pthread_mutex_unlock(&vsnd->common.transport_lock);
    return current;
}

static bool virtio_snd_capture_common_generation(virtio_snd_state_t *vsnd,
                                                 uint64_t *generation)
{
    unsigned status;
    bool current;

    if (!vsnd || !vsnd->common.initialized || !generation)
        return false;

    pthread_mutex_lock(&vsnd->common.transport_lock);
    status = virtio_snd_status_load(vsnd);
    current = !vsnd->common.reset_in_progress &&
              (status & VIRTIO_STATUS__DRIVER_OK) &&
              !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    if (current)
        *generation = vsnd->common.generation;
    pthread_mutex_unlock(&vsnd->common.transport_lock);
    return current;
}

static bool virtio_snd_queue_ready_for_actor(virtio_snd_state_t *vsnd,
                                             const struct virtq *queue)
{
    unsigned status;

    if (!vsnd || !queue || !queue->ready)
        return false;

    status = virtio_snd_status_load(vsnd);
    return !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) &&
           (status & VIRTIO_STATUS__DRIVER_OK);
}

static bool virtio_snd_begin_actor_completion(virtio_snd_state_t *vsnd,
                                              uint64_t actor_generation,
                                              uint64_t common_generation)
{
    bool common_current;

    if (!vsnd || !vsnd->actor_initialized || !vsnd->common.initialized)
        return false;

    pthread_mutex_lock(&vsnd->common.transport_lock);
    common_current = vsnd->common.generation == common_generation &&
                     !vsnd->common.reset_in_progress;
    if (!common_current) {
        pthread_mutex_unlock(&vsnd->common.transport_lock);
        return false;
    }

    if (!virtio_actor_begin_completion(&vsnd->actor, actor_generation)) {
        pthread_mutex_unlock(&vsnd->common.transport_lock);
        return false;
    }
    return true;
}

static void virtio_snd_end_actor_completion(virtio_snd_state_t *vsnd)
{
    if (!vsnd || !vsnd->actor_initialized)
        return;

    virtio_actor_end_completion(&vsnd->actor);
    pthread_mutex_unlock(&vsnd->common.transport_lock);
}

static void virtio_snd_set_fail_for_actor(virtio_snd_state_t *vsnd,
                                          uint64_t actor_generation,
                                          uint64_t common_generation)
{
    if (!virtio_snd_begin_actor_completion(vsnd, actor_generation,
                                           common_generation))
        return;
    virtio_snd_set_fail(vsnd);
    virtio_snd_end_actor_completion(vsnd);
}

static int virtio_snd_drain_queue(virtio_snd_state_t *vsnd,
                                  struct virtio_actor *actor,
                                  uint16_t queue_index,
                                  uint64_t actor_generation,
                                  uint64_t common_generation,
                                  bool flush_tx,
                                  bool *consumed)
{
    struct virtq *queue = &vsnd->common.queues[queue_index];

    for (;;) {
        struct virtq_iov readable[VSND_QUEUE_NUM_MAX];
        struct virtq_iov writable[VSND_QUEUE_NUM_MAX];
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        uint16_t available = 0;
        uint32_t used_len = 0;
        bool flush_tx_after_control = false;
        int ret;

        if (!virtio_snd_actor_generation_current(vsnd, actor, actor_generation))
            return 0;
        if (!virtio_snd_common_generation_current(vsnd, common_generation))
            return 0;

        ret = virtio_snd_queue_available(vsnd, queue, &available);
        if (ret < 0)
            return ret;
        if (available == 0)
            break;

        ret = virtq_pop(vsnd->common.dma, queue, &chain);
        if (!virtio_snd_actor_generation_current(vsnd, actor, actor_generation))
            return 0;
        if (!virtio_snd_common_generation_current(vsnd, common_generation))
            return 0;
        if (ret < 0)
            return ret;
        if (ret == 0)
            break;

        switch (queue_index) {
        case VSND_QUEUE_CTRL:
            ret = virtio_snd_process_control_chain(vsnd, &chain, &used_len,
                                                   &flush_tx_after_control);
            break;
        case VSND_QUEUE_TX:
            ret =
                virtio_snd_process_tx_chain(vsnd, &chain, flush_tx, &used_len);
            break;
        case VSND_QUEUE_RX:
            ret = virtio_snd_process_rx_chain(vsnd, &chain, &used_len);
            break;
        default:
            ret = -EINVAL;
            break;
        }

        if (!virtio_snd_actor_generation_current(vsnd, actor, actor_generation))
            return 0;
        if (!virtio_snd_common_generation_current(vsnd, common_generation))
            return 0;
        if (ret < 0)
            return ret;

        if (flush_tx_after_control) {
            bool tx_consumed = false;
            ret = virtio_snd_drain_queue(vsnd, actor, VSND_QUEUE_TX,
                                         actor_generation, common_generation,
                                         true, &tx_consumed);
            if (ret < 0)
                return ret;
            if (tx_consumed && virtio_snd_begin_actor_completion(
                                   vsnd, actor_generation, common_generation)) {
                struct virtq *tx = &vsnd->common.queues[VSND_QUEUE_TX];

                if (!virtq_interrupt_suppressed(vsnd->common.dma, tx))
                    virtio_irq_trigger(&vsnd->common.irq,
                                       VIRTIO_INT__USED_RING);
                virtio_snd_end_actor_completion(vsnd);
            }
        }

        if (!virtio_snd_begin_actor_completion(vsnd, actor_generation,
                                               common_generation))
            return 0;
        ret = virtq_add_used(vsnd->common.dma, queue, chain.head, used_len);
        if (ret < 0) {
            virtio_snd_set_fail(vsnd);
            virtio_snd_end_actor_completion(vsnd);
            return 0;
        }
        virtio_snd_end_actor_completion(vsnd);
        *consumed = true;
    }

    return 0;
}

static int virtio_snd_actor_drain_queue(void *opaque,
                                        struct virtio_actor *actor,
                                        uint16_t queue_index,
                                        uint64_t generation)
{
    virtio_snd_state_t *vsnd = opaque;
    bool consumed = false;
    uint64_t common_generation = 0;
    int ret;

    if (!vsnd || queue_index >= VSND_QUEUE_COUNT) {
        if (vsnd)
            virtio_snd_set_fail(vsnd);
        return 0;
    }
    if (queue_index == VSND_QUEUE_EVT)
        return 0;
    if (!virtio_snd_actor_generation_current(vsnd, actor, generation))
        return 0;
    if (!virtio_snd_queue_ready_for_actor(vsnd,
                                          &vsnd->common.queues[queue_index]))
        return 0;
    if (!virtio_snd_capture_common_generation(vsnd, &common_generation))
        return 0;

    ret = virtio_snd_drain_queue(vsnd, actor, queue_index, generation,
                                 common_generation, false, &consumed);
    if (ret < 0) {
        virtio_snd_set_fail_for_actor(vsnd, generation, common_generation);
        return 0;
    }

    if (consumed && virtio_snd_begin_actor_completion(vsnd, generation,
                                                      common_generation)) {
        struct virtq *queue = &vsnd->common.queues[queue_index];

        if (!virtq_interrupt_suppressed(vsnd->common.dma, queue))
            virtio_irq_trigger(&vsnd->common.irq, VIRTIO_INT__USED_RING);
        virtio_snd_end_actor_completion(vsnd);
    }
    return 0;
}

static bool virtio_snd_actor_queue_has_work(void *opaque,
                                            struct virtio_actor *actor,
                                            uint16_t queue_index,
                                            uint64_t generation)
{
    virtio_snd_state_t *vsnd = opaque;
    uint16_t available = 0;
    uint64_t common_generation = 0;
    int ret;

    if (!vsnd || queue_index >= VSND_QUEUE_COUNT ||
        queue_index == VSND_QUEUE_EVT)
        return false;
    if (!virtio_snd_actor_generation_current(vsnd, actor, generation))
        return false;
    if (!virtio_snd_queue_ready_for_actor(vsnd,
                                          &vsnd->common.queues[queue_index]))
        return false;
    if (!virtio_snd_capture_common_generation(vsnd, &common_generation))
        return false;

    ret = virtio_snd_queue_available(vsnd, &vsnd->common.queues[queue_index],
                                     &available);
    if (ret < 0) {
        virtio_snd_set_fail_for_actor(vsnd, generation, common_generation);
        return false;
    }
    return available != 0;
}

static void virtio_snd_actor_failed(void *opaque,
                                    struct virtio_actor *actor UNUSED)
{
    virtio_snd_state_t *vsnd = opaque;

    if (vsnd)
        virtio_snd_set_fail(vsnd);
}

static const struct virtio_actor_ops virtio_snd_actor_ops = {
    .drain_queue = virtio_snd_actor_drain_queue,
    .queue_has_work = virtio_snd_actor_queue_has_work,
    .on_failed = virtio_snd_actor_failed,
};

static int virtio_snd_activate(void *opaque,
                               const struct virtio_activation_context *ctx)
{
    virtio_snd_state_t *vsnd = opaque;
    int ret;

    (void) ctx;

    if (!vsnd || !vsnd->actor_initialized)
        return -EINVAL;

    ret = virtio_actor_start(&vsnd->actor);
    if (ret < 0 && ret != -EALREADY)
        return ret;

    ret = virtio_actor_enter_configuring(&vsnd->actor);
    if (ret < 0)
        return ret;
    return virtio_actor_activate(&vsnd->actor);
}

static int virtio_snd_prepare_reset(void *opaque,
                                    uint64_t old_generation,
                                    uint64_t new_generation)
{
    virtio_snd_state_t *vsnd = opaque;
    int ret;

    (void) old_generation;
    (void) new_generation;

    if (!vsnd)
        return 0;

    virtio_snd_mark_all_streams_releasing();
    if (!vsnd->actor_initialized) {
        virtio_snd_stop_all_streams(vsnd);
        return 0;
    }

    ret = virtio_actor_reset(&vsnd->actor);
    virtio_snd_stop_all_streams(vsnd);
    return ret;
}

static int virtio_snd_reset(void *opaque,
                            uint64_t old_generation,
                            uint64_t new_generation)
{
    (void) opaque;
    (void) old_generation;
    (void) new_generation;

    virtio_snd_reset_props();
    return 0;
}

static int virtio_snd_notify_queue(void *opaque,
                                   uint16_t queue_index,
                                   uint64_t generation)
{
    virtio_snd_state_t *vsnd = opaque;
    int ret;

    (void) generation;

    if (!vsnd || queue_index >= VSND_QUEUE_COUNT) {
        if (vsnd)
            virtio_snd_set_fail(vsnd);
        return -EINVAL;
    }

    ret = virtio_actor_notify_queue(&vsnd->actor, queue_index);
    if (ret == 0 || ret == -EAGAIN)
        return 0;

    virtio_snd_set_fail(vsnd);
    return ret;
}

static const struct virtio_device_ops virtio_snd_ops = {
    .activate = virtio_snd_activate,
    .prepare_reset = virtio_snd_prepare_reset,
    .reset = virtio_snd_reset,
    .notify_queue = virtio_snd_notify_queue,
    .read_config = virtio_snd_read_config,
    .write_config = virtio_snd_write_config,
};

static bool virtio_snd_load_width_bytes(uint8_t width, size_t *access_size)
{
    switch (width) {
    case RV_MEM_LW:
        *access_size = 4;
        return true;
    case RV_MEM_LBU:
    case RV_MEM_LB:
        *access_size = 1;
        return true;
    case RV_MEM_LHU:
    case RV_MEM_LH:
        *access_size = 2;
        return true;
    default:
        return false;
    }
}

static bool virtio_snd_store_width_bytes(uint8_t width, size_t *access_size)
{
    switch (width) {
    case RV_MEM_SW:
        *access_size = 4;
        return true;
    case RV_MEM_SB:
        *access_size = 1;
        return true;
    case RV_MEM_SH:
        *access_size = 2;
        return true;
    default:
        return false;
    }
}

static bool virtio_snd_is_config_access(uint32_t addr, size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(virtio_snd_config_t);

    if (access_size == 0 || addr < base || addr >= end)
        return false;
    return access_size <= end - addr;
}

void virtio_snd_read(hart_t *vm,
                     virtio_snd_state_t *vsnd,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_snd_load_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_snd_is_config_access(addr, access_size);
    if (addr >= (VIRTIO_Config << 2) && !is_cfg) {
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        return;
    }

    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
            return;
        }
    } else if (addr & (access_size - 1)) {
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        return;
    }

    ret = virtio_mmio_read(&vsnd->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

void virtio_snd_write(hart_t *vm,
                      virtio_snd_state_t *vsnd,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_snd_store_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_snd_is_config_access(addr, access_size);
    if (addr >= (VIRTIO_Config << 2) && !is_cfg) {
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        return;
    }

    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
    } else if (addr & (access_size - 1)) {
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        return;
    }

    ret = virtio_mmio_write(&vsnd->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

bool virtio_snd_irq_pending(virtio_snd_state_t *vsnd)
{
    return vsnd && virtio_irq_read_status(&vsnd->common.irq) != 0;
}

bool virtio_snd_init(virtio_snd_state_t *vsnd, emu_state_t *emu)
{
    static const uint16_t queue_max_sizes[] = {
        [VSND_QUEUE_CTRL] = VSND_QUEUE_NUM_MAX,
        [VSND_QUEUE_EVT] = VSND_QUEUE_NUM_MAX,
        [VSND_QUEUE_TX] = VSND_QUEUE_NUM_MAX,
        [VSND_QUEUE_RX] = VSND_QUEUE_NUM_MAX,
    };
    struct virtio_device_common_config common_config;
    PaError err;

    if (!vsnd || !emu) {
        fprintf(stderr, "Failed to initialize virtio-snd common device.\n");
        return false;
    }
    if (vsnd_dev_cnt >= VSND_DEV_CNT_MAX) {
        fprintf(stderr,
                "Exceeded the number of virtio-snd devices that can be "
                "allocated.\n");
        return false;
    }

    memset(vsnd, 0, sizeof(*vsnd));
    vsnd->ram = emu->ram;
    vsnd->priv = &vsnd_configs[vsnd_dev_cnt++];
    memset(PRIV(vsnd), 0, sizeof(*PRIV(vsnd)));
    PRIV(vsnd)->jacks = 1;
    PRIV(vsnd)->streams = 1;
    PRIV(vsnd)->chmaps = 1;
    PRIV(vsnd)->controls = 0;
    virtio_snd_reset_props();

    common_config = (struct virtio_device_common_config) {
        .emu = emu,
        .dma = &emu->ram_dma,
        .irq_source = SEMU_IRQ_SOURCE_VSND,
        .device_id = 25,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = VIRTIO_SND_F_VERSION_1,
        .required_features = VIRTIO_SND_F_VERSION_1,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &virtio_snd_ops,
        .opaque = vsnd,
    };

    if (virtio_device_common_init(&vsnd->common, &common_config) < 0) {
        vsnd->priv = NULL;
        vsnd_dev_cnt--;
        fprintf(stderr, "Failed to initialize virtio-snd common device.\n");
        return false;
    }

    if (virtio_actor_init(&vsnd->actor, &virtio_snd_actor_ops, vsnd,
                          ARRAY_SIZE(queue_max_sizes)) < 0) {
        virtio_device_common_destroy(&vsnd->common);
        vsnd->priv = NULL;
        vsnd_dev_cnt--;
        fprintf(stderr, "Failed to initialize virtio-snd actor.\n");
        return false;
    }
    vsnd->actor_initialized = true;

    err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "virtio-snd: Pa_Initialize: %s\n",
                Pa_GetErrorText(err));
        virtio_snd_destroy(vsnd);
        return false;
    }
    vsnd->portaudio_initialized = true;
    return true;
}

void virtio_snd_destroy(virtio_snd_state_t *vsnd)
{
    if (!vsnd)
        return;

    if (vsnd->actor_initialized) {
        virtio_snd_mark_all_streams_releasing();
        virtio_actor_stop(&vsnd->actor);
        virtio_actor_destroy(&vsnd->actor);
        vsnd->actor_initialized = false;
    }

    virtio_snd_stop_all_streams(vsnd);
    virtio_device_common_destroy(&vsnd->common);

    if (vsnd->portaudio_initialized) {
        Pa_Terminate();
        vsnd->portaudio_initialized = false;
    }
    if (vsnd->priv && vsnd_dev_cnt > 0)
        vsnd_dev_cnt--;
    vsnd->priv = NULL;
}
