#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "device.h"
#include "ram_access.h"
#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"
#include "virtio-actor.h"
#include "virtio-gpu.h"
#include "virtio-mmio.h"
#include "virtio.h"

#define VIRTIO_GPU_CMD_TRACE_ENABLED 0

#define VIRTIO_GPU_F_VERSION_1 (UINT64_C(1) << 32)

#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)
#define VIRTIO_GPU_F_EDID (1 << 1)
#define VIRTIO_GPU_F_CONTEXT_INIT (1 << 4)

#define VIRTIO_GPU_QUEUE_NUM_MAX 1024
#define VIRTIO_GPU_CONTROLQ 0
#define VIRTIO_GPU_CURSORQ 1

/* DMT usage macro */
#define EDID_BLOCK_SIZE 128U
#define DMT_BASE_WIDTH 1024U
#define DMT_BASE_HEIGHT 768U
#define DMT_BASE_PIXEL_CLOCK_10KHZ 6500U
#define DMT_BASE_H_BLANK 320U
#define DMT_BASE_H_FRONT 24U
#define DMT_BASE_H_SYNC 136U
#define DMT_BASE_V_BLANK 38U
#define DMT_BASE_V_FRONT 3U
#define DMT_BASE_V_SYNC 6U
#define DMT_BOUND_FIELD(field, max) \
    do {                            \
        if ((field) > (max))        \
            (field) = (max);        \
    } while (0)

#define PRIV(x) ((virtio_gpu_data_t *) x->priv)

#if VIRTIO_GPU_CMD_TRACE_ENABLED
#define VIRTIO_GPU_CMD_CASE(cmd, fn)                                 \
    case VIRTIO_GPU_CMD_##cmd:                                       \
        printf("(*) semu/virtio-gpu: %s\n", "VIRTIO_GPU_CMD_" #cmd); \
        g_virtio_gpu_backend.fn(vgpu, vq_desc, plen);                \
        break;
#else
#define VIRTIO_GPU_CMD_CASE(cmd, fn)                  \
    case VIRTIO_GPU_CMD_##cmd:                        \
        g_virtio_gpu_backend.fn(vgpu, vq_desc, plen); \
        break;
#endif

extern const struct virtio_gpu_cmd_backend g_virtio_gpu_backend;
static virtio_gpu_data_t virtio_gpu_data;

void *virtio_gpu_mem_guest_to_host(virtio_gpu_state_t *vgpu,
                                   uint32_t addr,
                                   uint32_t size)
{
    if (addr >= RAM_SIZE || size > RAM_SIZE - addr) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): guest address 0x%x size 0x%x out of bounds\n",
                __func__, addr, size);
        return NULL;
    }
    return (void *) ((uintptr_t) vgpu->ram + addr);
}

static inline unsigned virtio_gpu_status_load(virtio_gpu_state_t *vgpu)
{
    return atomic_load_explicit(&vgpu->common.status, memory_order_acquire);
}

void virtio_gpu_set_fail(virtio_gpu_state_t *vgpu)
{
    unsigned status = virtio_gpu_status_load(vgpu);

    virtio_device_common_set_needs_reset(&vgpu->common);
    if (status & VIRTIO_STATUS__DRIVER_OK)
        virtio_irq_trigger(&vgpu->common.irq, VIRTIO_INT__CONF_CHANGE);
}

bool virtio_gpu_actor_drain_current(virtio_gpu_state_t *vgpu)
{
    return vgpu && vgpu->actor_initialized &&
           virtio_actor_generation(&vgpu->actor) ==
               vgpu->actor_drain_generation;
}

bool virtio_gpu_begin_actor_completion(virtio_gpu_state_t *vgpu)
{
    return vgpu && vgpu->actor_initialized &&
           virtio_actor_begin_completion(&vgpu->actor,
                                         vgpu->actor_drain_generation);
}

int virtio_gpu_end_actor_completion(virtio_gpu_state_t *vgpu)
{
    if (!vgpu || !vgpu->actor_initialized)
        return -EINVAL;
    return virtio_actor_end_completion(&vgpu->actor);
}

void *virtio_gpu_get_request(virtio_gpu_state_t *vgpu,
                             struct virtq_desc *vq_desc,
                             size_t request_size)
{
    if ((vq_desc[0].flags & VIRTIO_DESC_F_WRITE) ||
        vq_desc[0].len < request_size || request_size > UINT32_MAX)
        return NULL;

    return virtio_gpu_mem_guest_to_host(vgpu, vq_desc[0].addr,
                                        (uint32_t) request_size);
}

const struct virtq_desc *virtio_gpu_get_response_desc(
    struct virtq_desc *vq_desc,
    size_t response_size)
{
    /* The common virtq adapter stores all device-readable segments first and
     * then all writable response segments. The first writable segment is the
     * response buffer for current 2D commands; a too-small writable descriptor
     * is malformed, so do not skip past it looking for another response.
     */
    if (response_size <= UINT32_MAX) {
        for (size_t i = 1; i < VIRTIO_GPU_MAX_DESC; i++) {
            if (!(vq_desc[i].flags & VIRTIO_DESC_F_WRITE))
                continue;
            if (vq_desc[i].len < response_size)
                break;
            return &vq_desc[i];
        }
    }

    return NULL;
}

uint32_t virtio_gpu_write_ctrl_response(
    virtio_gpu_state_t *vgpu,
    const struct virtio_gpu_ctrl_hdr *request,
    const struct virtq_desc *response_desc,
    uint32_t type)
{
    if (response_desc->len < sizeof(struct virtio_gpu_ctrl_hdr))
        return 0;

    struct virtio_gpu_ctrl_hdr *response = virtio_gpu_mem_guest_to_host(
        vgpu, response_desc->addr, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response)
        return 0;

    memset(response, 0, sizeof(*response));
    response->type = type;

    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->flags = VIRTIO_GPU_FLAG_FENCE;
        response->fence_id = request->fence_id;
    }

    return sizeof(*response);
}

/* 'virtio_gpu' protocol handlers */
void virtio_gpu_get_display_info_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    struct virtio_gpu_ctrl_hdr *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_resp_disp_info));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_resp_disp_info *response = virtio_gpu_mem_guest_to_host(
        vgpu, response_desc->addr, sizeof(struct virtio_gpu_resp_disp_info));
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;

    /* 'GET_DISPLAY_INFO' exposes scanouts as the 'pmodes[]' array, so the array
     * index is the guest-visible 'scanout_id' used by later requests such as
     * 'SET_SCANOUT' and 'GET_EDID'.
     *
     * The spec describes 'pmodes[]' as per-scanout information but does not
     * spell out this mapping as a separate rule. semu follows the implicit
     * model where 'pmodes[i]' describes scanout ID 'i' because later requests
     * only carry a 'scanout_id', and Linux does the same when it copies
     * 'resp->pmodes[i]' into 'outputs[i]' and later sends 'output->index' in
     * 'SET_SCANOUT'. See 'virtgpu_vq.c' and 'virtgpu_display.c' for more
     * details.
     */
    int scanout_num = PRIV(vgpu)->num_scanouts;
    for (int i = 0; i < scanout_num; i++) {
        response->pmodes[i].r.width = PRIV(vgpu)->scanouts[i].width;
        response->pmodes[i].r.height = PRIV(vgpu)->scanouts[i].height;
        response->pmodes[i].enabled = PRIV(vgpu)->scanouts[i].enabled;
    }

    if (request->flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->fence_id;
    }
    *plen = sizeof(*response);
}

static uint8_t virtio_gpu_generate_edid_checksum(uint8_t *edid, size_t size)
{
    /* Check EDID 1.4 Section 3.11, Table 3.40 notes 2 and 3: byte 7Fh must
     * make the modulo-256 sum of all 128 base EDID bytes equal 00h.
     */
    uint8_t sum = 0;

    for (size_t i = 0; i < size; i++)
        sum += edid[i];

    return 0x100 - sum;
}

static uint16_t virtio_gpu_edid_pixels_to_mm(uint32_t pixels)
{
    /* Check EDID 1.4 Sections 3.6.2 and 3.10.2: base screen size is stored in
     * centimeters, while detailed timing image size is stored in millimeters.
     * Estimate virtual display size at 100 DPI.
     */
    uint32_t mm = ((uint64_t) pixels * 254U + 500U) / 1000U;

    if (mm == 0)
        mm = 1;
    if (mm > 4095)
        mm = 4095;

    return mm;
}

static uint8_t virtio_gpu_edid_mm_to_cm(uint16_t mm)
{
    /* Check EDID 1.4 Section 3.6.2: base screen size fields are centimeters. */
    uint32_t cm = (mm + 5U) / 10U;

    if (cm == 0)
        cm = 1;
    if (cm > 255)
        cm = 255;

    return cm;
}

static void virtio_gpu_edid_set_srgb_chromaticity(uint8_t *edid)
{
    /* Check EDID 1.4 Section 3.7: sRGB chromaticity coordinates in EDID
     * 10-bit fixed-point form, value = round(coordinate * 1024). The white
     * point is D65.
     */
    const uint16_t red_x = 655;   /* round(0.640 * 1024) */
    const uint16_t red_y = 338;   /* round(0.330 * 1024) */
    const uint16_t green_x = 307; /* round(0.300 * 1024) */
    const uint16_t green_y = 614; /* round(0.600 * 1024) */
    const uint16_t blue_x = 154;  /* round(0.150 * 1024) */
    const uint16_t blue_y = 61;   /* round(0.060 * 1024) */
    const uint16_t white_x = 320; /* round(0.313 * 1024) */
    const uint16_t white_y = 337; /* round(0.329 * 1024) */

    edid[25] = ((red_x & 0x3) << 6) | ((red_y & 0x3) << 4) |
               ((green_x & 0x3) << 2) | (green_y & 0x3);
    edid[26] = ((blue_x & 0x3) << 6) | ((blue_y & 0x3) << 4) |
               ((white_x & 0x3) << 2) | (white_y & 0x3);
    edid[27] = red_x >> 2;
    edid[28] = red_y >> 2;
    edid[29] = green_x >> 2;
    edid[30] = green_y >> 2;
    edid[31] = blue_x >> 2;
    edid[32] = blue_y >> 2;
    edid[33] = white_x >> 2;
    edid[34] = white_y >> 2;
}

static void virtio_gpu_edid_set_detailed_timing(uint8_t *desc,
                                                uint32_t width,
                                                uint32_t height,
                                                uint16_t width_mm,
                                                uint16_t height_mm)
{
    /* Check EDID 1.4 Section 3.10.2: detailed timing descriptor layout. */
    uint32_t h_blank;           /* Horizontal blanking pixels. */
    uint32_t h_front;           /* Horizontal front porch pixels. */
    uint32_t h_sync;            /* Horizontal sync pulse width. */
    uint32_t v_blank;           /* Vertical blanking lines. */
    uint32_t v_front;           /* Vertical front porch lines. */
    uint32_t v_sync;            /* Vertical sync pulse width. */
    uint32_t pixel_clock_10khz; /* Pixel clock in 10 kHz units. */

    if (width == DMT_BASE_WIDTH && height == DMT_BASE_HEIGHT) {
        /* VESA DMT 1024x768@60Hz, also advertised in the base EDID established
         * timings field. EDID stores pixel clock in 10 kHz units, so 6500
         * means 65.00 MHz.
         */
        pixel_clock_10khz = DMT_BASE_PIXEL_CLOCK_10KHZ;
        h_blank = DMT_BASE_H_BLANK;
        h_front = DMT_BASE_H_FRONT;
        h_sync = DMT_BASE_H_SYNC;
        v_blank = DMT_BASE_V_BLANK;
        v_front = DMT_BASE_V_FRONT;
        v_sync = DMT_BASE_V_SYNC;
    } else {
        /* Fallback only for future multi-mode or non-default scanouts. The
         * current machine registers one 1024x768 scanout, so this path is not
         * reachable in the default build. Scale porch/sync proportions from
         * the VESA DMT 1024x768@60Hz timing instead of inventing ad hoc
         * ratios.
         */
        h_blank = ((uint64_t) width * DMT_BASE_H_BLANK + DMT_BASE_WIDTH / 2U) /
                  DMT_BASE_WIDTH;
        h_front = ((uint64_t) width * DMT_BASE_H_FRONT + DMT_BASE_WIDTH / 2U) /
                  DMT_BASE_WIDTH;
        h_sync = ((uint64_t) width * DMT_BASE_H_SYNC + DMT_BASE_WIDTH / 2U) /
                 DMT_BASE_WIDTH;
        if (h_front == 0)
            h_front = 1;
        if (h_sync == 0)
            h_sync = 1;
        if (h_blank <= h_front + h_sync) {
            /* Keep front porch and sync pulse inside the blanking interval so
             * the remaining pixels form the back porch.
             */
            h_blank = h_front + h_sync + 1U;
        }

        v_blank =
            ((uint64_t) height * DMT_BASE_V_BLANK + DMT_BASE_HEIGHT / 2U) /
            DMT_BASE_HEIGHT;
        v_front =
            ((uint64_t) height * DMT_BASE_V_FRONT + DMT_BASE_HEIGHT / 2U) /
            DMT_BASE_HEIGHT;
        v_sync = ((uint64_t) height * DMT_BASE_V_SYNC + DMT_BASE_HEIGHT / 2U) /
                 DMT_BASE_HEIGHT;
        if (v_front == 0)
            v_front = 1;
        if (v_sync == 0)
            v_sync = 1;
        if (v_blank <= v_front + v_sync)
            v_blank = v_front + v_sync + 1U;

        /* Pixel clock = refresh rate * horizontal total * vertical total.
         * Divide by 10000 because the descriptor stores the clock in 10 kHz
         * units. The +5000 rounds to the nearest 10 kHz.
         */
        pixel_clock_10khz = (60U * ((uint64_t) width + h_blank) *
                                 ((uint64_t) height + v_blank) +
                             5000U) /
                            10000U;
        if (pixel_clock_10khz > 0xffffU)
            pixel_clock_10khz = 0xffffU;
    }

    /* Clamp fields to the bit widths defined by Table 3.21:
     * active/blanking/image-size fields are 12-bit, horizontal sync fields are
     * 10-bit, and vertical sync fields are 6-bit.
     */
    DMT_BOUND_FIELD(width, 4095U);
    DMT_BOUND_FIELD(height, 4095U);
    DMT_BOUND_FIELD(h_blank, 4095U);
    DMT_BOUND_FIELD(h_front, 1023U);
    DMT_BOUND_FIELD(h_sync, 1023U);
    DMT_BOUND_FIELD(v_blank, 4095U);
    DMT_BOUND_FIELD(v_front, 63U);
    DMT_BOUND_FIELD(v_sync, 63U);

    /* Bytes 0-1: pixel clock, little-endian, in 10 kHz units. */
    desc[0] = pixel_clock_10khz & 0xff;
    desc[1] = (pixel_clock_10khz >> 8) & 0xff;

    /* Bytes 2-4: horizontal active and blanking, each split as low 8 bits plus
     * high 4 bits packed into byte 4.
     */
    desc[2] = width & 0xff;
    desc[3] = h_blank & 0xff;
    desc[4] = ((width >> 8) << 4) | (h_blank >> 8);

    /* Bytes 5-7: vertical active and blanking, using the same 12-bit packing
     * pattern as the horizontal fields.
     */
    desc[5] = height & 0xff;
    desc[6] = v_blank & 0xff;
    desc[7] = ((height >> 8) << 4) | (v_blank >> 8);

    /* Bytes 8-11: sync offsets and pulse widths. Horizontal fields are 10-bit;
     * vertical fields are 6-bit and share byte 10 for their low nibbles.
     */
    desc[8] = h_front & 0xff;
    desc[9] = h_sync & 0xff;
    desc[10] = ((v_front & 0xf) << 4) | (v_sync & 0xf);
    desc[11] = ((h_front >> 8) << 6) | ((h_sync >> 8) << 4) |
               ((v_front >> 4) << 2) | (v_sync >> 4);

    /* Bytes 12-14: displayed image size in millimeters, again as two 12-bit
     * fields packed as low 8 bits plus high 4 bits.
     */
    desc[12] = width_mm & 0xff;
    desc[13] = height_mm & 0xff;
    desc[14] = ((width_mm >> 8) << 4) | (height_mm >> 8);

    /* Bytes 15-16: horizontal and vertical border, unused for this display. */
    desc[15] = 0;
    desc[16] = 0;

    /* Byte 17: non-interlaced, no stereo, digital separate sync, negative H/V
     * sync polarity.
     */
    desc[17] = 0x18;
}

/* EDID data follows "VESA ENHANCED EXTENDED DISPLAY IDENTIFICATION DATA
 * STANDARD" (defines EDID Structure Version 1, Revision 4).
 */
static void virtio_gpu_generate_edid(uint8_t *edid,
                                     uint32_t width,
                                     uint32_t height)
{
    /* Check EDID 1.4 Section 3.1: base EDID block layout. */
    if (width == 0)
        width = SCREEN_WIDTH;
    if (height == 0)
        height = SCREEN_HEIGHT;

    uint16_t width_mm = virtio_gpu_edid_pixels_to_mm(width);
    uint16_t height_mm = virtio_gpu_edid_pixels_to_mm(height);

    memset(edid, 0, EDID_BLOCK_SIZE);

    /* Check EDID 1.4 Section 3.3: EDID header. */
    edid[0] = 0x00;
    edid[1] = 0xff;
    edid[2] = 0xff;
    edid[3] = 0xff;
    edid[4] = 0xff;
    edid[5] = 0xff;
    edid[6] = 0xff;
    edid[7] = 0x00;

    /* Check EDID 1.4 Section 3.4.1: ID Manufacturer Name, stored as a
     * 3-character PNPID in 5-bit compressed ASCII.
     */
    char manufacture[3] = {'T', 'W', 'N'};

    /* Vendor ID uses 2 bytes to store 3 characters, where 'A' starts as 1 */
    uint16_t vendor_id = ((((manufacture[0] - '@') & 0b11111) << 10) |
                          (((manufacture[1] - '@') & 0b11111) << 5) |
                          (((manufacture[2] - '@') & 0b11111) << 0));
    /* Convert vendor ID to big-endian order */
    edid[8] = vendor_id >> 8;
    edid[9] = vendor_id & 0xff;

    /* Check EDID 1.4 Sections 3.4.2 and 3.4.3: product code and serial
     * number, all zeros if unused.
     */
    memset(&edid[10], 0, sizeof(uint16_t) + sizeof(uint32_t));

    /* Check EDID 1.4 Section 3.4.4: week of manufacture, 0 if unused. */
    edid[16] = 0;
    /* Check EDID 1.4 Section 3.4.4: year of manufacture starts from 1990. */
    edid[17] = 2023 - 1990;

    /* Check EDID 1.4 Section 3.5: version 1, revision 4. */
    edid[18] = 1; /* Version number */
    edid[19] = 4; /* Revision number */

    /* Check EDID 1.4 Section 3.6.1: video input definition. */
    uint8_t signal_interface = 0b1 << 7;  /* digital */
    uint8_t color_bit_depth = 0b010 << 4; /* 8 bits per primary color */
    uint8_t interface_type = 0b101;       /* DisplayPort is supported */
    edid[20] = signal_interface | color_bit_depth | interface_type;

    /* Check EDID 1.4 Section 3.6.2: screen size or aspect ratio. */
    edid[21] = virtio_gpu_edid_mm_to_cm(width_mm);
    edid[22] = virtio_gpu_edid_mm_to_cm(height_mm);

    /* Check EDID 1.4 Section 3.6.3: gamma value. */
    edid[23] = 120; /* 2.20 */

    /* Check EDID 1.4 Section 3.6.4: feature support. */
    uint8_t power_management = 0 << 4; /* standby, suspend and active-off
                                        * modes are not supported
                                        */
    uint8_t color_type = 0 << 3;       /* RGB 4:4:4 */
    uint8_t other_flags = 0b110;       /* [2]: sRGB as default color space
                                        * [1]: Preferred timing mode with native
                                        * format       [0]: Non-continuous frequency
                                        */
    edid[24] = power_management | color_type | other_flags;

    virtio_gpu_edid_set_srgb_chromaticity(edid);

    /* Check EDID 1.4 Section 3.8: established timings. These are the default
     * timings defined by the VESA. Each bit represents 1 configuration. For
     * now, we enable the timing configurations of 1024x768@60Hz only.
     */
    edid[35] = 0b00000000;
    edid[36] = (width == DMT_BASE_WIDTH && height == DMT_BASE_HEIGHT)
                   ? 0b00001000
                   : 0b00000000;
    edid[37] = 0b00000000;

    /* Check EDID 1.4 Section 3.9: standard timings. The 16 bytes from
     * edid[38] to edid[53] hold eight 2-byte timing identifiers. Mark every
     * standard timing slot unused.
     */
    memset(&edid[38], 0x01, 16);

    /* Check EDID 1.4 Sections 3.10.1 and 3.10.2: first detailed timing
     * descriptor is the preferred timing mode, here the native scanout mode at
     * 60Hz.
     */
    virtio_gpu_edid_set_detailed_timing(&edid[54], width, height, width_mm,
                                        height_mm);

    /* Check EDID 1.4 Sections 3.10 and 3.10.3.11: mark remaining 18-byte
     * descriptor slots unused with Dummy Descriptor tag 10h.
     */
    for (size_t desc = 72; desc < 126; desc += 18)
        edid[desc + 3] = 0x10;

    /* Check EDID 1.4 Section 3.11: extension block count. */
    edid[126] = 0; /* No other extension blocks are defined */

    /* Check EDID 1.4 Section 3.11: checksum of the base EDID block. */
    edid[EDID_BLOCK_SIZE - 1U] =
        virtio_gpu_generate_edid_checksum(edid, EDID_BLOCK_SIZE - 1U);
}

void virtio_gpu_get_edid_handler(virtio_gpu_state_t *vgpu,
                                 struct virtq_desc *vq_desc,
                                 uint32_t *plen)
{
    struct virtio_gpu_cmd_get_edid *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_cmd_get_edid));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_resp_edid));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (request->scanout >= PRIV(vgpu)->num_scanouts ||
        !PRIV(vgpu)->scanouts[request->scanout].enabled) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): invalid scanout id %u\n",
                __func__, request->scanout);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    const struct virtio_gpu_scanout_info *scanout =
        &PRIV(vgpu)->scanouts[request->scanout];

    struct virtio_gpu_resp_edid *response = virtio_gpu_mem_guest_to_host(
        vgpu, response_desc->addr, sizeof(struct virtio_gpu_resp_edid));
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_EDID;
    response->size = EDID_BLOCK_SIZE; /* One base EDID block. */
    virtio_gpu_generate_edid((uint8_t *) response->edid, scanout->width,
                             scanout->height);

    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }
    *plen = sizeof(*response);
}

void virtio_gpu_cmd_undefined_handler(virtio_gpu_state_t *vgpu,
                                      struct virtq_desc *vq_desc,
                                      uint32_t *plen)
{
    struct virtio_gpu_ctrl_hdr *header = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!header) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    fprintf(stderr,
            VIRTIO_GPU_LOG_PREFIX
            "%s(): unsupported VirtIO-GPU command type "
            "%u\n",
            __func__, header->type);

    virtio_gpu_set_fail(vgpu);
    *plen = 0;
}

static bool virtio_gpu_queue_available(virtio_gpu_state_t *vgpu,
                                       const struct virtq *queue,
                                       uint16_t *available)
{
    uint16_t avail_idx;
    uint16_t delta;

    if (!queue || !queue->ready || !available)
        return false;

    if (!ram_dma_read(vgpu->common.dma, queue->driver_addr + 2, &avail_idx,
                      sizeof(avail_idx))) {
        virtio_gpu_set_fail(vgpu);
        return false;
    }

    delta = (uint16_t) (avail_idx - queue->last_avail);
    if (delta > queue->queue_size) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): avail index advanced by %u entries, exceeds queue "
                "size %u\n",
                __func__, (unsigned) delta, (unsigned) queue->queue_size);
        virtio_gpu_set_fail(vgpu);
        return false;
    }

    *available = delta;
    return true;
}

static int virtio_gpu_append_iov_desc(struct virtq_desc *vq_desc,
                                      size_t capacity,
                                      size_t total,
                                      size_t *count,
                                      const struct virtq_iov *iov,
                                      bool writable)
{
    uint16_t flags = writable ? VIRTIO_DESC_F_WRITE : 0;

    if (*count >= capacity || !iov)
        return -1;
    if (iov->addr > UINT32_MAX)
        return -1;
    if (*count + 1 < total)
        flags |= VIRTIO_DESC_F_NEXT;

    vq_desc[*count] = (struct virtq_desc) {
        .addr = iov->addr,
        .len = iov->len,
        .flags = flags,
        .next = *count + 1 < total ? (uint16_t) (*count + 1) : 0,
    };
    (*count)++;
    return 0;
}

static int virtio_gpu_chain_to_descs(const struct virtq_chain *chain,
                                     struct virtq_desc *vq_desc,
                                     size_t capacity)
{
    size_t count = 0;
    size_t total;

    if (!chain || !vq_desc || chain->readable_count == 0)
        return -1;
    total = chain->readable_count + chain->writable_count;
    if (total == 0 || total > capacity)
        return -1;

    for (size_t i = 0; i < chain->readable_count; i++) {
        if (virtio_gpu_append_iov_desc(vq_desc, capacity, total, &count,
                                       &chain->readable[i], false) < 0)
            return -1;
    }
    for (size_t i = 0; i < chain->writable_count; i++) {
        if (virtio_gpu_append_iov_desc(vq_desc, capacity, total, &count,
                                       &chain->writable[i], true) < 0)
            return -1;
    }

    return 0;
}

static int virtio_gpu_desc_handler(virtio_gpu_state_t *vgpu,
                                   int queue_index,
                                   const struct virtq_chain *chain,
                                   uint32_t *plen)
{
    struct virtq_desc vq_desc[VIRTIO_GPU_MAX_DESC] = {0};

    if (virtio_gpu_chain_to_descs(chain, vq_desc, ARRAY_SIZE(vq_desc)) < 0) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return -1;
    }

    struct virtio_gpu_ctrl_hdr *header = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!header) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return -1;
    }

    bool is_cursor_cmd = header->type == VIRTIO_GPU_CMD_UPDATE_CURSOR ||
                         header->type == VIRTIO_GPU_CMD_MOVE_CURSOR;
    if ((queue_index == VIRTIO_GPU_CONTROLQ && is_cursor_cmd) ||
        (queue_index == VIRTIO_GPU_CURSORQ && !is_cursor_cmd)) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return -1;
    }

    /* Process the command */
    switch (header->type) {
        /* 2D commands */
        VIRTIO_GPU_CMD_CASE(GET_DISPLAY_INFO, get_display_info)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_2D, resource_create_2d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_UNREF, resource_unref)
        VIRTIO_GPU_CMD_CASE(SET_SCANOUT, set_scanout)
        VIRTIO_GPU_CMD_CASE(RESOURCE_FLUSH, resource_flush)
        VIRTIO_GPU_CMD_CASE(TRANSFER_TO_HOST_2D, transfer_to_host_2d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_ATTACH_BACKING, resource_attach_backing)
        VIRTIO_GPU_CMD_CASE(RESOURCE_DETACH_BACKING, resource_detach_backing)
        VIRTIO_GPU_CMD_CASE(GET_CAPSET_INFO, get_capset_info)
        VIRTIO_GPU_CMD_CASE(GET_CAPSET, get_capset)
        VIRTIO_GPU_CMD_CASE(GET_EDID, get_edid)
        VIRTIO_GPU_CMD_CASE(RESOURCE_ASSIGN_UUID, resource_assign_uuid)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_BLOB, resource_create_blob)
        VIRTIO_GPU_CMD_CASE(SET_SCANOUT_BLOB, set_scanout_blob)
        /* 3D commands */
        VIRTIO_GPU_CMD_CASE(CTX_CREATE, ctx_create)
        VIRTIO_GPU_CMD_CASE(CTX_DESTROY, ctx_destroy)
        VIRTIO_GPU_CMD_CASE(CTX_ATTACH_RESOURCE, ctx_attach_resource)
        VIRTIO_GPU_CMD_CASE(CTX_DETACH_RESOURCE, ctx_detach_resource)
        VIRTIO_GPU_CMD_CASE(RESOURCE_CREATE_3D, resource_create_3d)
        VIRTIO_GPU_CMD_CASE(TRANSFER_TO_HOST_3D, transfer_to_host_3d)
        VIRTIO_GPU_CMD_CASE(TRANSFER_FROM_HOST_3D, transfer_from_host_3d)
        VIRTIO_GPU_CMD_CASE(SUBMIT_3D, submit_3d)
        VIRTIO_GPU_CMD_CASE(RESOURCE_MAP_BLOB, resource_map_blob)
        VIRTIO_GPU_CMD_CASE(RESOURCE_UNMAP_BLOB, resource_unmap_blob)
        VIRTIO_GPU_CMD_CASE(UPDATE_CURSOR, update_cursor)
        VIRTIO_GPU_CMD_CASE(MOVE_CURSOR, move_cursor)
    default:
        virtio_gpu_cmd_undefined_handler(vgpu, vq_desc, plen);
        return -1;
    }

    return 0;
}

static bool virtio_gpu_actor_generation_current(struct virtio_actor *actor,
                                                uint64_t generation)
{
    return actor && virtio_actor_generation(actor) == generation;
}

static bool virtio_gpu_queue_ready_for_actor(virtio_gpu_state_t *vgpu,
                                             struct virtq *queue)
{
    unsigned status = virtio_gpu_status_load(vgpu);

    if (status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return false;
    if ((status & VIRTIO_STATUS__DRIVER_OK) && queue && queue->ready)
        return true;

    virtio_gpu_set_fail(vgpu);
    return false;
}

static int virtio_gpu_actor_drain_queue(void *opaque,
                                        struct virtio_actor *actor,
                                        uint16_t queue_index,
                                        uint64_t generation)
{
    virtio_gpu_state_t *vgpu = opaque;
    struct virtq *queue;
    struct virtq_iov readable[VIRTIO_GPU_QUEUE_NUM_MAX];
    struct virtq_iov writable[VIRTIO_GPU_QUEUE_NUM_MAX];
    bool consumed = false;

    if (!vgpu || queue_index >= vgpu->common.num_queues) {
        if (vgpu)
            virtio_gpu_set_fail(vgpu);
        return 0;
    }

    queue = &vgpu->common.queues[queue_index];
    vgpu->actor_drain_generation = generation;

    if (!virtio_gpu_actor_generation_current(actor, generation))
        return 0;
    if (!virtio_gpu_queue_ready_for_actor(vgpu, queue))
        return 0;

    for (;;) {
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        uint16_t available;
        uint32_t len = 0;
        int ret;

        if (!virtio_gpu_actor_generation_current(actor, generation))
            return 0;
        if (!virtio_gpu_queue_available(vgpu, queue, &available))
            return 0;
        if (available == 0)
            break;

        ret = virtq_pop(vgpu->common.dma, queue, &chain);
        if (ret < 0) {
            virtio_gpu_set_fail(vgpu);
            return 0;
        }
        if (ret == 0)
            break;

        if (!virtio_gpu_actor_generation_current(actor, generation))
            return 0;
        if (virtio_gpu_desc_handler(vgpu, queue_index, &chain, &len) != 0)
            return 0;

        if (!virtio_actor_begin_completion(actor, generation))
            return 0;
        ret = virtq_add_used(vgpu->common.dma, queue, chain.head, len);
        virtio_actor_end_completion(actor);
        if (ret < 0) {
            virtio_gpu_set_fail(vgpu);
            return 0;
        }
        consumed = true;

        if (virtio_gpu_status_load(vgpu) & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
            break;
    }

    if (consumed && virtio_actor_begin_completion(actor, generation)) {
        if (!virtq_interrupt_suppressed(vgpu->common.dma, queue))
            virtio_irq_trigger(&vgpu->common.irq, VIRTIO_INT__USED_RING);
        virtio_actor_end_completion(actor);
    }
    return 0;
}

static bool virtio_gpu_actor_queue_has_work(void *opaque,
                                            struct virtio_actor *actor,
                                            uint16_t queue_index,
                                            uint64_t generation)
{
    virtio_gpu_state_t *vgpu = opaque;
    uint16_t available = 0;

    if (!vgpu || queue_index >= vgpu->common.num_queues)
        return false;
    if (!virtio_gpu_actor_generation_current(actor, generation))
        return false;
    if (!virtio_gpu_queue_ready_for_actor(vgpu,
                                          &vgpu->common.queues[queue_index]))
        return false;
    if (!virtio_gpu_queue_available(vgpu, &vgpu->common.queues[queue_index],
                                    &available))
        return false;
    return available != 0;
}

static const struct virtio_actor_ops virtio_gpu_actor_ops = {
    .drain_queue = virtio_gpu_actor_drain_queue,
    .queue_has_work = virtio_gpu_actor_queue_has_work,
};

static bool virtio_gpu_config_range_valid(uint32_t offset, uint32_t size)
{
    return size != 0 && offset < sizeof(struct virtio_gpu_config) &&
           size <= sizeof(struct virtio_gpu_config) - offset;
}

static inline bool virtio_gpu_is_config_access(uint32_t addr,
                                               size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(struct virtio_gpu_config);

    if (access_size == 0 || addr < base || addr >= end)
        return false;
    return access_size <= end - addr;
}

static uint32_t virtio_gpu_read_config(void *opaque,
                                       uint32_t offset,
                                       uint32_t size)
{
    virtio_gpu_state_t *vgpu = opaque;
    struct virtio_gpu_config config = {
        .events_read = 0,
        .events_clear = 0,
        .num_scanouts = PRIV(vgpu)->num_scanouts,
        .num_capsets = 0,
    };
    uint32_t value = 0;

    if (!virtio_gpu_config_range_valid(offset, size))
        return 0;

    memcpy(&value, (uint8_t *) &config + offset, size);
    return value;
}

static void virtio_gpu_write_config(void *opaque,
                                    uint32_t offset,
                                    uint32_t size,
                                    uint32_t value)
{
    (void) opaque;
    (void) offset;
    (void) size;
    (void) value;
    /* No display events are currently implemented, so events_clear is a no-op.
     */
}

static bool virtio_gpu_load_width_bytes(uint8_t width, size_t *access_size)
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

static bool virtio_gpu_store_width_bytes(uint8_t width, size_t *access_size)
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

static bool virtio_gpu_config_write_allowed(uint32_t addr, size_t size)
{
    uint32_t offset = addr - (VIRTIO_Config << 2);
    uint32_t field = offsetof(struct virtio_gpu_config, events_clear);

    return virtio_gpu_config_range_valid(offset, (uint32_t) size) &&
           offset >= field && offset < field + sizeof(uint32_t) &&
           size <= field + sizeof(uint32_t) - offset;
}

void virtio_gpu_read(hart_t *vm,
                     virtio_gpu_state_t *vgpu,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_gpu_load_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_gpu_is_config_access(addr, access_size);
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

    ret = virtio_mmio_read(&vgpu->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

void virtio_gpu_write(hart_t *vm,
                      virtio_gpu_state_t *vgpu,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_gpu_store_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_gpu_is_config_access(addr, access_size);
    if (addr >= (VIRTIO_Config << 2) && !is_cfg) {
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        return;
    }

    if (!is_cfg) {
        if (access_size != 4 || (addr & 0x3)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
    } else {
        if (addr & (access_size - 1)) {
            vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
            return;
        }
        if (!virtio_gpu_config_write_allowed(addr, access_size)) {
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
            return;
        }
    }

    ret = virtio_mmio_write(&vgpu->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

bool virtio_gpu_irq_pending(virtio_gpu_state_t *vgpu)
{
    return virtio_irq_read_status(&vgpu->common.irq) != 0;
}

static int virtio_gpu_activate(void *opaque,
                               const struct virtio_activation_context *ctx)
{
    virtio_gpu_state_t *vgpu = opaque;
    int ret;

    (void) ctx;

    ret = virtio_actor_start(&vgpu->actor);
    if (ret < 0 && ret != -EALREADY)
        return ret;

    ret = virtio_actor_enter_configuring(&vgpu->actor);
    if (ret < 0)
        return ret;
    return virtio_actor_activate(&vgpu->actor);
}

static int virtio_gpu_prepare_reset(void *opaque,
                                    uint64_t old_generation,
                                    uint64_t new_generation)
{
    virtio_gpu_state_t *vgpu = opaque;

    (void) old_generation;
    (void) new_generation;

    return virtio_actor_reset(&vgpu->actor);
}

static int virtio_gpu_reset(void *opaque,
                            uint64_t old_generation,
                            uint64_t new_generation)
{
    virtio_gpu_state_t *vgpu = opaque;

    (void) old_generation;
    (void) new_generation;

    if (g_virtio_gpu_backend.reset)
        g_virtio_gpu_backend.reset(vgpu);
    return 0;
}

static int virtio_gpu_notify_queue(void *opaque,
                                   uint16_t queue_index,
                                   uint64_t generation)
{
    virtio_gpu_state_t *vgpu = opaque;
    int ret;

    (void) generation;

    if (queue_index != VIRTIO_GPU_CONTROLQ &&
        queue_index != VIRTIO_GPU_CURSORQ) {
        virtio_gpu_set_fail(vgpu);
        return -EINVAL;
    }

    ret = virtio_actor_notify_queue(&vgpu->actor, queue_index);
    if (ret == -EAGAIN)
        return 0;
    if (ret < 0) {
        virtio_gpu_set_fail(vgpu);
        return ret;
    }
    return 0;
}

static const struct virtio_device_ops virtio_gpu_ops = {
    .activate = virtio_gpu_activate,
    .prepare_reset = virtio_gpu_prepare_reset,
    .reset = virtio_gpu_reset,
    .notify_queue = virtio_gpu_notify_queue,
    .read_config = virtio_gpu_read_config,
    .write_config = virtio_gpu_write_config,
};

void virtio_gpu_init(virtio_gpu_state_t *vgpu, emu_state_t *emu)
{
    static const uint16_t queue_max_sizes[] = {
        [VIRTIO_GPU_CONTROLQ] = VIRTIO_GPU_QUEUE_NUM_MAX,
        [VIRTIO_GPU_CURSORQ] = VIRTIO_GPU_QUEUE_NUM_MAX,
    };
    static bool initialized = false;
    struct virtio_device_common_config config;

    if (initialized) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): only one virtio-gpu instance is supported\n",
                __func__);
        exit(EXIT_FAILURE);
    }

    memset(vgpu, 0, sizeof(*vgpu));
    memset(&virtio_gpu_data, 0, sizeof(virtio_gpu_data));
    vgpu->ram = emu->ram;
    vgpu->priv = &virtio_gpu_data;
    virtio_gpu_sw_backend_init(vgpu);

    config = (struct virtio_device_common_config) {
        .emu = emu,
        .dma = &emu->ram_dma,
        .irq_source = SEMU_IRQ_SOURCE_VGPU,
        .device_id = 16,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = VIRTIO_GPU_F_EDID | VIRTIO_GPU_F_VERSION_1,
        .required_features = VIRTIO_GPU_F_VERSION_1,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &virtio_gpu_ops,
        .opaque = vgpu,
    };

    if (virtio_device_common_init(&vgpu->common, &config) < 0) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): failed to initialize common VirtIO transport\n",
                __func__);
        exit(EXIT_FAILURE);
    }

    if (virtio_actor_init(&vgpu->actor, &virtio_gpu_actor_ops, vgpu,
                          ARRAY_SIZE(queue_max_sizes)) < 0) {
        virtio_device_common_destroy(&vgpu->common);
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): failed to initialize VirtIO actor\n",
                __func__);
        exit(EXIT_FAILURE);
    }
    vgpu->actor_initialized = true;

    initialized = true;
}

uint32_t virtio_gpu_register_scanout(virtio_gpu_state_t *vgpu,
                                     uint32_t width,
                                     uint32_t height)
{
    int scanout_num = PRIV(vgpu)->num_scanouts;
    if (scanout_num >= VIRTIO_GPU_MAX_SCANOUTS) {
        /* Registration is init-only today. Return an error instead if scanout
         * creation becomes dynamic or guest-triggered.
         */
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): exceeded scanout maximum number\n",
                __func__);
        exit(EXIT_FAILURE);
    }

    PRIV(vgpu)->scanouts[scanout_num].width = width;
    PRIV(vgpu)->scanouts[scanout_num].height = height;
    PRIV(vgpu)->scanouts[scanout_num].enabled = 1;
    PRIV(vgpu)->scanouts[scanout_num].primary_resource_id = 0;
    PRIV(vgpu)->scanouts[scanout_num].cursor_resource_id = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_x = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_y = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_w = 0;
    PRIV(vgpu)->scanouts[scanout_num].src_h = 0;

    /* 'scanout_num' will match the guest-visible 'scanout_id'. See
     * 'virtio_gpu_get_display_info_handler()' above for how that index is
     * exposed to the guest and later reused in 'SET_SCANOUT'/'GET_EDID'.
     */
    PRIV(vgpu)->num_scanouts++;

    return (uint32_t) scanout_num;
}
