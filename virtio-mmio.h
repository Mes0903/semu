#pragma once

#include <stdint.h>

#include "virtio-device.h"

#define VIRTIO_MMIO_MAGIC_VALUE 0x74726976U
#define VIRTIO_MMIO_VERSION_VALUE 2U

int virtio_mmio_read(struct virtio_device_common *common,
                     uint32_t byte_offset,
                     uint8_t width,
                     uint32_t *value);
int virtio_mmio_write(struct virtio_device_common *common,
                      uint32_t byte_offset,
                      uint8_t width,
                      uint32_t value);
