#pragma once

#include <pthread.h>

#if SEMU_HAS(VIRTIONET)
#include "netdev.h"
#endif
#include "mmio-bus.h"
#include "ram_access.h"
#include "riscv.h"
#include "virtio-device.h"
#include "virtio.h"
#include "vm-lifecycle.h"

/* RAM */

#define RAM_SIZE (512 * 1024 * 1024)
#define DTB_SIZE (1 * 1024 * 1024)
#define INITRD_SIZE (8 * 1024 * 1024)

#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 768

void ram_read(hart_t *core,
              uint32_t *mem,
              const uint32_t addr,
              const uint8_t width,
              uint32_t *value);

void ram_write(hart_t *core,
               uint32_t *mem,
               const uint32_t addr,
               const uint8_t width,
               const uint32_t value);

/* PLIC */

typedef struct plic_state {
    uint32_t masked;
    uint32_t ip;     /* support 32 interrupt sources only */
    uint32_t ie[32]; /* support 32 sources to 32 contexts only */
    /* state of input interrupt lines (level-triggered), set by environment */
    uint32_t active;
} plic_state_t;

bool plic_set_source_level(vm_t *vm,
                           plic_state_t *plic,
                           uint32_t source_id,
                           bool level);
void plic_update_interrupts(vm_t *vm, plic_state_t *plic);
void plic_read(hart_t *core,
               plic_state_t *plic,
               uint32_t addr,
               uint8_t width,
               uint32_t *value);
void plic_write(hart_t *core,
                plic_state_t *plic,
                uint32_t addr,
                uint8_t width,
                uint32_t value);
/* UART */

#define IRQ_UART 1
#define IRQ_UART_BIT (1 << IRQ_UART)

typedef struct {
    uint8_t dll, dlh;                  /**< divisor (ignored) */
    uint8_t lcr;                       /**< UART config */
    uint8_t ier;                       /**< interrupt config */
    uint8_t current_int, pending_ints; /**< interrupt status */
    /* other output signals, loopback mode (ignored) */
    uint8_t mcr;
    /* I/O handling */
    int in_fd, out_fd;
    bool in_ready;
    /* Output buffering */
    uint8_t out_buf[128];
    uint8_t out_buf_len;
} u8250_state_t;

void u8250_update_interrupts(u8250_state_t *uart);
void u8250_read(hart_t *core,
                u8250_state_t *uart,
                uint32_t addr,
                uint8_t width,
                uint32_t *value);
void u8250_write(hart_t *core,
                 u8250_state_t *uart,
                 uint32_t addr,
                 uint8_t width,
                 uint32_t value);
void u8250_check_ready(u8250_state_t *uart);
void u8250_flush_out(u8250_state_t *uart);
void capture_keyboard_input();

/* virtio-net */

#if SEMU_HAS(VIRTIONET)
#define IRQ_VNET 2
#define IRQ_VNET_BIT (1 << IRQ_VNET)

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
    bool fd_ready;
} virtio_net_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    /* queue config */
    uint32_t QueueSel;
    virtio_net_queue_t queues[2];
    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;
    /* supplied by environment */
    netdev_t peer;
    uint32_t *ram;
    /* implementation-specific */
    void *priv;
} virtio_net_state_t;

void virtio_net_read(hart_t *core,
                     virtio_net_state_t *vnet,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);
void virtio_net_write(hart_t *core,
                      virtio_net_state_t *vnet,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);
void virtio_net_refresh_queue(virtio_net_state_t *vnet);

void virtio_net_recv_from_peer(void *peer);

bool virtio_net_init(virtio_net_state_t *vnet, const char *name);
#endif /* SEMU_HAS(VIRTIONET) */

/* VirtIO-Block */

#if SEMU_HAS(VIRTIOBLK)

#define IRQ_VBLK 3
#define IRQ_VBLK_BIT (1 << IRQ_VBLK)

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
} virtio_blk_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    /* queue config */
    uint32_t QueueSel;
    virtio_blk_queue_t queues[2];
    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;
    /* supplied by environment */
    uint32_t *ram;
    uint32_t *disk;
    /* implementation-specific */
    void *priv;
} virtio_blk_state_t;

void virtio_blk_read(hart_t *vm,
                     virtio_blk_state_t *vblk,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);

void virtio_blk_write(hart_t *vm,
                      virtio_blk_state_t *vblk,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);

uint32_t *virtio_blk_init(virtio_blk_state_t *vblk, char *disk_file);
#endif /* SEMU_HAS(VIRTIOBLK) */

/* VirtIO-RNG */

#if SEMU_HAS(VIRTIORNG)

#define IRQ_VRNG 4
#define IRQ_VRNG_BIT (1 << IRQ_VRNG)

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
} virtio_rng_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    /* queue config */
    uint32_t QueueSel;
    virtio_rng_queue_t queues[1];
    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;
    /* supplied by environment */
    uint32_t *ram;
} virtio_rng_state_t;

void virtio_rng_read(hart_t *vm,
                     virtio_rng_state_t *rng,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);

void virtio_rng_write(hart_t *vm,
                      virtio_rng_state_t *vrng,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);

void virtio_rng_init(void);
#endif /* SEMU_HAS(VIRTIORNG) */

/* VirtIO Input */

#if SEMU_HAS(VIRTIOINPUT)

#define IRQ_VINPUT_KEYBOARD 7
#define IRQ_VINPUT_KEYBOARD_BIT (1 << IRQ_VINPUT_KEYBOARD)

#define IRQ_VINPUT_MOUSE 8
#define IRQ_VINPUT_MOUSE_BIT (1 << IRQ_VINPUT_MOUSE)

typedef struct {
    struct virtio_device_common common;
    /* supplied by environment */
    uint32_t *ram;
    /* implementation-specific */
    void *priv;
} virtio_input_state_t;

void virtio_input_read(hart_t *vm,
                       virtio_input_state_t *vinput,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t *value);

void virtio_input_write(hart_t *vm,
                        virtio_input_state_t *vinput,
                        uint32_t addr,
                        uint8_t width,
                        uint32_t value);

void virtio_input_init(virtio_input_state_t *vinput,
                       emu_state_t *emu,
                       enum semu_irq_source irq_source);

/* Drain translated host window events and update guest-visible virtio-input
 * device state. Must be called from the emulator thread.
 */
void virtio_input_drain_host_events(void);

/* Returns true if the device has a pending interrupt. Safe to call from
 * the emulator thread without holding any lock internally.
 */
bool virtio_input_irq_pending(virtio_input_state_t *vinput);
#endif /* SEMU_HAS(VIRTIOINPUT) */

/* VirtIO-GPU */

#if SEMU_HAS(VIRTIOGPU)

#define IRQ_VGPU 9
#define IRQ_VGPU_BIT (1 << IRQ_VGPU)

typedef struct {
    struct virtio_device_common common;
    /* supplied by environment */
    uint32_t *ram;
    /* implementation-specific */
    void *priv;
} virtio_gpu_state_t;

void virtio_gpu_read(hart_t *vm,
                     virtio_gpu_state_t *vgpu,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);

void virtio_gpu_write(hart_t *vm,
                      virtio_gpu_state_t *vgpu,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);

/* Initializes the process-wide virtio-gpu singleton. semu currently supports
 * one in-process GPU instance; a second call is fatal.
 */
void virtio_gpu_init(virtio_gpu_state_t *vgpu, emu_state_t *emu);
uint32_t virtio_gpu_register_scanout(virtio_gpu_state_t *vgpu,
                                     uint32_t width,
                                     uint32_t height);
bool virtio_gpu_irq_pending(virtio_gpu_state_t *vgpu);
#endif /* SEMU_HAS(VIRTIOGPU) */

/* ACLINT MTIMER */
typedef struct {
    /* A MTIMER device has two separate base addresses: one for the MTIME
     * register and another for the MTIMECMP registers.
     *
     * The MTIME register is a 64-bit read-write register that contains the
     * number of cycles counted based on a fixed reference frequency.
     *
     * The MTIMECMP registers are 'per-HART' 64-bit read-write registers. It
     * contains the MTIME register value at which machine-level timer interrupt
     * is to be triggered for the corresponding HART.
     *
     * Up to 4095 MTIMECMP registers can exist, corresponding to 4095 HARTs in
     * the system.
     *
     * For more details, please refer to the register map at:
     * https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc#21-register-map
     */
    _Atomic uint64_t *mtimecmp;
    uint32_t n_hart;
    semu_timer_t mtime;
} mtimer_state_t;

void aclint_mtimer_update_interrupts(hart_t *hart, mtimer_state_t *mtimer);
void aclint_mtimer_read(hart_t *hart,
                        mtimer_state_t *mtimer,
                        uint32_t addr,
                        uint8_t width,
                        uint32_t *value);
void aclint_mtimer_write(hart_t *hart,
                         mtimer_state_t *mtimer,
                         uint32_t addr,
                         uint8_t width,
                         uint32_t value);

/* ACLINT MSWI */
typedef struct {
    /* The MSWI device provides machine-level IPI functionality for a set of
     * HARTs on a RISC-V platform. It has an IPI register (MSIP) for each HART
     * connected to the MSWI device.
     *
     * Up to 4095 MSIP registers can be used, corresponding to 4095 HARTs in the
     * system. The 4096th MSIP register is reserved for future use.
     *
     * For more details, please refer to the register map at:
     * https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc#31-register-map
     */
    _Atomic uint32_t *msip;
    uint32_t n_hart;
} mswi_state_t;

void aclint_mswi_update_interrupts(hart_t *hart, mswi_state_t *mswi);
void aclint_mswi_read(hart_t *hart,
                      mswi_state_t *mswi,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t *value);
void aclint_mswi_write(hart_t *hart,
                       mswi_state_t *mswi,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t value);

/* ACLINT SSWI */
typedef struct {
    /* The SSWI device provides supervisor-level IPI functionality for a set of
     * HARTs on a RISC-V platform. It provides a register to set an IPI
     * (SETSSIP) for each HART connected to the SSWI device.
     *
     * Up to 4095 SETSSIP registers can be used, corresponding to 4095 HARTs in
     * the system. The 4096th SETSSIP register is reserved for future use.
     *
     * For more details, please refer to the register map at:
     * https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc#41-register-map
     */
    _Atomic uint32_t *ssip;
    uint32_t n_hart;
} sswi_state_t;

void aclint_sswi_update_interrupts(hart_t *hart, sswi_state_t *sswi);
void aclint_swi_update_interrupts(hart_t *hart,
                                  mswi_state_t *mswi,
                                  sswi_state_t *sswi);
void aclint_sswi_read(hart_t *hart,
                      sswi_state_t *sswi,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t *value);
void aclint_sswi_write(hart_t *hart,
                       sswi_state_t *sswi,
                       uint32_t addr,
                       uint8_t width,
                       uint32_t value);

/* VirtIO-Sound */

#if SEMU_HAS(VIRTIOSND)
#define IRQ_VSND 5
#define IRQ_VSND_BIT (1 << IRQ_VSND)

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
} virtio_snd_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;
    /* queue config */
    uint32_t QueueSel;
    virtio_snd_queue_t queues[4];
    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;
    /* supplied by environment */
    uint32_t *ram;
    /* implementation-specific */
    void *priv;
} virtio_snd_state_t;

void virtio_snd_read(hart_t *core,
                     virtio_snd_state_t *vsnd,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t *value);

void virtio_snd_write(hart_t *core,
                      virtio_snd_state_t *vsnd,
                      uint32_t addr,
                      uint8_t width,
                      uint32_t value);

bool virtio_snd_init(virtio_snd_state_t *vsnd);
#endif /* SEMU_HAS(VIRTIOSND) */

/* VirtIO-File-System */

#if SEMU_HAS(VIRTIOFS)
#define IRQ_VFS 6
#define IRQ_VFS_BIT (1 << IRQ_VFS)

typedef struct inode_map_entry {
    uint64_t ino;
    char *path;
    struct inode_map_entry *next;
} inode_map_entry;

typedef struct {
    uint32_t QueueNum;
    uint32_t QueueDesc;
    uint32_t QueueAvail;
    uint32_t QueueUsed;
    uint16_t last_avail;
    bool ready;
} virtio_fs_queue_t;

typedef struct {
    /* feature negotiation */
    uint32_t DeviceFeaturesSel;
    uint32_t DriverFeatures;
    uint32_t DriverFeaturesSel;

    /* queue config */
    uint32_t QueueSel;
    virtio_fs_queue_t queues[3];

    /* status */
    uint32_t Status;
    uint32_t InterruptStatus;

    /* guest memory base */
    uint32_t *ram;

    char *mount_tag; /* guest sees this tag */
    char *shared_dir;

    inode_map_entry *inode_map;

    /* optional implementation-specific */
    void *priv;
} virtio_fs_state_t;

/* MMIO read/write */
void virtio_fs_read(hart_t *core,
                    virtio_fs_state_t *vfs,
                    uint32_t addr,
                    uint8_t width,
                    uint32_t *value);

void virtio_fs_write(hart_t *core,
                     virtio_fs_state_t *vfs,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t value);

bool virtio_fs_init(virtio_fs_state_t *vfs, char *mtag, char *dir);

#endif /* SEMU_HAS(VIRTIOFS) */

/* memory mapping */
typedef enum {
    SEMU_RFENCE_NONE = 0,
    SEMU_RFENCE_I,
    SEMU_RFENCE_VMA,
} semu_rfence_type_t;

typedef struct {
    _Atomic bool initialized;
    _Atomic int type;
    uint32_t start_addr;
    uint32_t size;
    uint32_t asid;
    _Atomic int32_t pending_count;
    pthread_mutex_t issue_mutex;
    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_cond;
} rfence_request_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} hart_wait_t;

typedef struct emu_state {
    int exit_code;
    bool debug;
    _Atomic bool stopped;
    struct semu_vm_lifecycle lifecycle;
    uint32_t *ram;
    ram_dma_t ram_dma;
    uint32_t *disk;
    vm_t vm;
    struct semu_mmio_bus mmio_bus;
    plic_state_t plic;
    pthread_mutex_t plic_lock;
    u8250_state_t uart;
    pthread_mutex_t uart_lock;
#if SEMU_HAS(VIRTIONET)
    virtio_net_state_t vnet;
    pthread_mutex_t vnet_lock;
#endif
#if SEMU_HAS(VIRTIOBLK)
    virtio_blk_state_t vblk;
    pthread_mutex_t vblk_lock;
#endif
#if SEMU_HAS(VIRTIORNG)
    virtio_rng_state_t vrng;
    pthread_mutex_t vrng_lock;
#endif
#if SEMU_HAS(VIRTIOSND)
    virtio_snd_state_t vsnd;
    pthread_mutex_t vsnd_lock;
#endif
#if SEMU_HAS(VIRTIOFS)
    virtio_fs_state_t vfs;
    pthread_mutex_t vfs_lock;
#endif
#if SEMU_HAS(VIRTIOINPUT)
    virtio_input_state_t vkeyboard;
    pthread_mutex_t vkeyboard_lock;
    virtio_input_state_t vmouse;
    pthread_mutex_t vmouse_lock;
#endif
#if SEMU_HAS(VIRTIOGPU)
    virtio_gpu_state_t vgpu;
    pthread_mutex_t vgpu_lock;
#endif
#if SEMU_HAS(VIRTIOINPUT) || SEMU_HAS(VIRTIOGPU)
    /* Use self-pipe trick to unblock the emulator loop when the window backend
     * has queued work, such as input events or window shutdown. When all harts
     * are idle, 'semu_run()' can call 'poll(-1)' and block indefinitely
     * waiting for timer or UART events. The window-event thread has no way to
     * wake that blocked 'poll()' other than writing to a file descriptor it is
     * watching.
     *
     * 'wake_fd[0]' (read end) is added to 'pfds[]' so 'poll()' monitors it.
     * 'wake_fd[1]' (write end) is handed to the window backend, which
     * writes one byte when backend work arrives to make 'wake_fd[0]'
     * readable and return 'poll()' immediately.
     */
    int wake_fd[2];
#endif
    /* ACLINT */
    mtimer_state_t mtimer;
    pthread_mutex_t mtimer_lock;
    mswi_state_t mswi;
    pthread_mutex_t mswi_lock;
    sswi_state_t sswi;
    pthread_mutex_t sswi_lock;

    pthread_t *hart_threads;
    pthread_t io_thread;
    bool io_thread_created;
    _Atomic bool threaded_fatal;
    hart_wait_t *hart_wait;

    _Atomic uint32_t peripheral_update_ctr;
    rfence_request_t rfence;

    /* The fields used for debug mode */
    bool is_interrupted;
    int curr_cpuid;
} emu_state_t;
