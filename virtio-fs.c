#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "device.h"
#include "fuse.h"
#include "ram_access.h"
#include "riscv_private.h"
#include "virtio-irq.h"
#include "virtio-mmio.h"
#include "virtq.h"

/* SEMU currently only supports a single virtio-fs device. Although virtio-fs
 * allows multiple mount points if supported by the device, we limit to one for
 * simplicity.
 */
#define VFS_DEV_CNT_MAX 1

#define VFS_FEATURES_0 0
#define VFS_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */
#define VIRTIO_FS_F_VERSION_1 (UINT64_C(1) << 32)
#define VFS_QUEUE_NUM_MAX 1024
#define VFS_QUEUE (vfs->queues[vfs->QueueSel])
#define NUM_REQUEST_QUEUES_ADDR 0x49
#define VFS_MAX_CHAIN_IOVS 4

#define PRIV(x) ((struct virtio_fs_config *) (x)->priv)

PACKED(struct virtio_fs_config {
    char tag[36];
    uint32_t num_request_queues;
    uint32_t notify_buf_size; /* ignored */
});

typedef struct {
    DIR *dir;
    char *path;
} dir_handle_t;

static char *virtio_fs_strdup(const char *src);

typedef enum {
    VIRTIO_FS_HANDLE_FILE,
    VIRTIO_FS_HANDLE_DIR,
} virtio_fs_handle_type_t;

struct virtio_fs_handle_entry {
    uint64_t id;
    virtio_fs_handle_type_t type;
    int fd;
    DIR *dir;
    char *path;
    virtio_fs_handle_entry *next;
};

static void virtio_fs_destroy_handle(virtio_fs_handle_entry *handle)
{
    if (!handle)
        return;

    if (handle->type == VIRTIO_FS_HANDLE_FILE && handle->fd >= 0)
        close(handle->fd);
    if (handle->type == VIRTIO_FS_HANDLE_DIR && handle->dir)
        closedir(handle->dir);
    free(handle->path);
    free(handle);
}

static uint64_t virtio_fs_alloc_handle_id(virtio_fs_state_t *vfs)
{
    uint64_t id;

    if (!vfs)
        return 0;
    if (vfs->next_handle_id == 0)
        vfs->next_handle_id = 1;

    id = vfs->next_handle_id++;
    if (vfs->next_handle_id == 0)
        vfs->next_handle_id = 1;
    return id;
}

static int virtio_fs_add_file_handle(virtio_fs_state_t *vfs,
                                     int fd,
                                     uint64_t *id)
{
    virtio_fs_handle_entry *handle;

    if (!vfs || fd < 0 || !id)
        return -EINVAL;

    handle = calloc(1, sizeof(*handle));
    if (!handle)
        return -ENOMEM;

    handle->id = virtio_fs_alloc_handle_id(vfs);
    if (handle->id == 0) {
        free(handle);
        return -ENOMEM;
    }
    handle->type = VIRTIO_FS_HANDLE_FILE;
    handle->fd = fd;
    handle->next = vfs->handles;
    vfs->handles = handle;
    *id = handle->id;
    return 0;
}

static int virtio_fs_add_dir_handle(virtio_fs_state_t *vfs,
                                    DIR *dir,
                                    const char *path,
                                    uint64_t *id)
{
    virtio_fs_handle_entry *handle;

    if (!vfs || !dir || !path || !id)
        return -EINVAL;

    handle = calloc(1, sizeof(*handle));
    if (!handle)
        return -ENOMEM;

    handle->id = virtio_fs_alloc_handle_id(vfs);
    if (handle->id == 0) {
        free(handle);
        return -ENOMEM;
    }
    handle->type = VIRTIO_FS_HANDLE_DIR;
    handle->fd = -1;
    handle->dir = dir;
    handle->path = virtio_fs_strdup(path);
    if (!handle->path) {
        free(handle);
        return -ENOMEM;
    }
    handle->next = vfs->handles;
    vfs->handles = handle;
    *id = handle->id;
    return 0;
}

static virtio_fs_handle_entry *virtio_fs_find_handle(
    virtio_fs_state_t *vfs,
    uint64_t id,
    virtio_fs_handle_type_t type)
{
    for (virtio_fs_handle_entry *handle = vfs ? vfs->handles : NULL; handle;
         handle = handle->next) {
        if (handle->id == id && handle->type == type)
            return handle;
    }
    return NULL;
}

static virtio_fs_handle_entry *virtio_fs_remove_handle(
    virtio_fs_state_t *vfs,
    uint64_t id,
    virtio_fs_handle_type_t type)
{
    virtio_fs_handle_entry **cursor;

    if (!vfs || id == 0)
        return NULL;

    cursor = &vfs->handles;
    while (*cursor) {
        virtio_fs_handle_entry *handle = *cursor;

        if (handle->id == id && handle->type == type) {
            *cursor = handle->next;
            handle->next = NULL;
            return handle;
        }
        cursor = &handle->next;
    }
    return NULL;
}

static void virtio_fs_close_all_handles(virtio_fs_state_t *vfs)
{
    virtio_fs_handle_entry *handle;

    if (!vfs)
        return;

    handle = vfs->handles;
    vfs->handles = NULL;
    while (handle) {
        virtio_fs_handle_entry *next = handle->next;

        handle->next = NULL;
        virtio_fs_destroy_handle(handle);
        handle = next;
    }
}

inode_map_entry *find_inode_path(inode_map_entry *head, uint64_t ino)
{
    while (head) {
        if (head->ino == ino)
            return head;
        head = head->next;
    }
    return NULL;
}

static struct virtio_fs_config vfs_configs[VFS_DEV_CNT_MAX];
static int vfs_dev_cnt = 0;

static inline unsigned virtio_fs_status_load(virtio_fs_state_t *vfs)
{
    if (!vfs)
        return 0;
    if (vfs->common.initialized)
        return atomic_load_explicit(&vfs->common.status, memory_order_acquire);
    return vfs->Status;
}

static bool virtio_fs_config_range_valid(uint32_t offset, uint32_t size)
{
    return size != 0 && offset < sizeof(struct virtio_fs_config) &&
           size <= sizeof(struct virtio_fs_config) - offset;
}

static uint32_t virtio_fs_read_config(void *opaque,
                                      uint32_t offset,
                                      uint32_t size)
{
    virtio_fs_state_t *vfs = opaque;
    uint32_t value = 0;

    if (!vfs || !virtio_fs_config_range_valid(offset, size))
        return 0;

    memcpy(&value, (uint8_t *) PRIV(vfs) + offset, size);
    return value;
}

static int virtio_fs_queue_available(virtio_fs_state_t *vfs,
                                     struct virtq *queue,
                                     uint16_t *available)
{
    uint16_t avail_idx;
    uint16_t delta;

    if (!vfs || !queue || !queue->ready || !available)
        return -EINVAL;

    if (!ram_dma_read(vfs->common.dma, queue->driver_addr + 2, &avail_idx,
                      sizeof(avail_idx)))
        return -EFAULT;

    delta = (uint16_t) (avail_idx - queue->last_avail);
    if (delta > queue->queue_size)
        return -EINVAL;

    *available = delta;
    return 0;
}

static bool virtio_fs_read_iov(virtio_fs_state_t *vfs,
                               const struct virtq_iov *iov,
                               void *dst,
                               guest_size_t len)
{
    if (!vfs || !iov || !dst || iov->len < len)
        return false;
    return ram_dma_read(vfs->common.dma, iov->addr, dst, len);
}

static bool virtio_fs_write_iov_at(virtio_fs_state_t *vfs,
                                   const struct virtq_iov *iov,
                                   guest_size_t offset,
                                   const void *src,
                                   guest_size_t len)
{
    if (!vfs || !iov || !src || offset > iov->len || len > iov->len - offset)
        return false;
    return ram_dma_write(vfs->common.dma, iov->addr + offset, src, len);
}

static bool virtio_fs_write_iov(virtio_fs_state_t *vfs,
                                const struct virtq_iov *iov,
                                const void *src,
                                guest_size_t len)
{
    return virtio_fs_write_iov_at(vfs, iov, 0, src, len);
}

static bool virtio_fs_write_response_header(virtio_fs_state_t *vfs,
                                            const struct virtq_iov *iov,
                                            uint64_t unique,
                                            int32_t error,
                                            uint32_t payload_len,
                                            uint32_t *used_len)
{
    struct vfs_resp_header header_resp = {
        .out =
            {
                .len = sizeof(struct fuse_out_header) + payload_len,
                .error = error,
                .unique = unique,
            },
    };

    if (!used_len)
        return false;
    if (!virtio_fs_write_iov(vfs, iov, &header_resp, sizeof(header_resp)))
        return false;
    *used_len = header_resp.out.len;
    return true;
}

static void virtio_fs_fill_attr(struct fuse_attr *attr, const struct stat *st)
{
    memset(attr, 0, sizeof(*attr));
    attr->ino = st->st_ino;
    attr->size = st->st_size;
    attr->blocks = st->st_blocks;
    attr->atime = st->st_atime;
    attr->mtime = st->st_mtime;
    attr->ctime = st->st_ctime;
    attr->mode = st->st_mode;
    attr->nlink = st->st_nlink;
    attr->uid = st->st_uid;
    attr->gid = st->st_gid;
    attr->blksize = st->st_blksize;
}

static const char *virtio_fs_path_for_inode(virtio_fs_state_t *vfs,
                                            uint64_t inode)
{
    if (inode == 1)
        return vfs->shared_dir;

    inode_map_entry *entry = find_inode_path(vfs->inode_map, inode);
    return entry ? entry->path : NULL;
}

static bool virtio_fs_lookup_name_valid(const char *name, size_t len)
{
    if (!name || len == 0)
        return false;
    if (memchr(name, '\0', len) || memchr(name, '/', len))
        return false;
    if (len == 1 && name[0] == '.')
        return false;
    if (len == 2 && name[0] == '.' && name[1] == '.')
        return false;
    return true;
}

static int virtio_fs_process_fuse_init(virtio_fs_state_t *vfs,
                                       const struct virtq_chain *chain,
                                       const struct vfs_req_header *req,
                                       uint32_t *used_len)
{
    struct fuse_init_out init_out = {0};

    if (!vfs || !chain || !req || !used_len)
        return -EINVAL;
    if (chain->readable_count < 2 || chain->writable_count < 2)
        return -EINVAL;
    if (chain->writable[1].len < sizeof(init_out))
        return -EINVAL;

    init_out.major = 7;
    init_out.minor = 41;
    init_out.max_readahead = 0x10000;
    init_out.flags = FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_DO_READDIRPLUS;
    init_out.max_background = 64;
    init_out.congestion_threshold = 32;
    init_out.max_write = 0x131072;
    init_out.time_gran = 1;

    if (!virtio_fs_write_iov(vfs, &chain->writable[1], &init_out,
                             sizeof(init_out)) ||
        !virtio_fs_write_response_header(vfs, &chain->writable[0],
                                         req->in.unique, 0, sizeof(init_out),
                                         used_len))
        return -EFAULT;

    return 0;
}

static int virtio_fs_process_getattr(virtio_fs_state_t *vfs,
                                     const struct virtq_chain *chain,
                                     const struct vfs_req_header *req,
                                     uint32_t *used_len)
{
    struct fuse_attr_out outattr = {0};
    const char *target_path;
    struct stat st;

    if (!vfs || !chain || !req || !used_len || chain->writable_count < 2)
        return -EINVAL;

    target_path = virtio_fs_path_for_inode(vfs, req->in.nodeid);
    if (!target_path)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOENT, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    if (stat(target_path, &st) < 0)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -errno, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    outattr.attr_valid = 60;
    virtio_fs_fill_attr(&outattr.attr, &st);
    if (!virtio_fs_write_iov(vfs, &chain->writable[1], &outattr,
                             sizeof(outattr)) ||
        !virtio_fs_write_response_header(vfs, &chain->writable[0],
                                         req->in.unique, 0, sizeof(outattr),
                                         used_len))
        return -EFAULT;
    return 0;
}

static int virtio_fs_process_opendir(virtio_fs_state_t *vfs,
                                     const struct virtq_chain *chain,
                                     const struct vfs_req_header *req,
                                     uint32_t *used_len)
{
    struct fuse_open_out open_out = {0};
    inode_map_entry *entry;
    uint64_t handle_id;
    DIR *dir;
    int ret;

    if (!vfs || !chain || !req || !used_len || chain->writable_count < 2)
        return -EINVAL;

    entry = find_inode_path(vfs->inode_map, req->in.nodeid);
    if (!entry)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOENT, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    dir = opendir(entry->path);
    if (!dir)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -errno, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    ret = virtio_fs_add_dir_handle(vfs, dir, entry->path, &handle_id);
    if (ret < 0) {
        closedir(dir);
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, ret, 0, used_len)
                   ? 0
                   : -EFAULT;
    }

    open_out.fh = handle_id;
    if (!virtio_fs_write_iov(vfs, &chain->writable[1], &open_out,
                             sizeof(open_out)) ||
        !virtio_fs_write_response_header(vfs, &chain->writable[0],
                                         req->in.unique, 0, sizeof(open_out),
                                         used_len)) {
        virtio_fs_handle_entry *handle =
            virtio_fs_remove_handle(vfs, handle_id, VIRTIO_FS_HANDLE_DIR);
        virtio_fs_destroy_handle(handle);
        return -EFAULT;
    }
    return 0;
}

static int virtio_fs_process_readdirplus(virtio_fs_state_t *vfs,
                                         const struct virtq_chain *chain,
                                         const struct vfs_req_header *req,
                                         uint32_t *used_len)
{
    struct fuse_read_in read_in;
    virtio_fs_handle_entry *handle;
    uint8_t *payload;
    size_t offset = 0;

    if (!vfs || !chain || !req || !used_len || chain->readable_count < 2 ||
        chain->writable_count < 2)
        return -EINVAL;
    if (!virtio_fs_read_iov(vfs, &chain->readable[1], &read_in,
                            sizeof(read_in)))
        return -EFAULT;

    handle = virtio_fs_find_handle(vfs, read_in.fh, VIRTIO_FS_HANDLE_DIR);
    if (!handle || !handle->dir)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -EBADF, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    payload = calloc(1, chain->writable[1].len ? chain->writable[1].len : 1);
    if (!payload)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOMEM, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    rewinddir(handle->dir);
    for (;;) {
        struct dirent *entry = readdir(handle->dir);
        char *full_path;
        size_t dir_len;
        size_t name_len;
        size_t record_size;
        size_t record_aligned;
        struct stat st;
        struct fuse_direntplus *direntplus;

        if (!entry)
            break;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        dir_len = strlen(handle->path);
        name_len = strlen(entry->d_name);
        record_size = sizeof(struct fuse_direntplus) + name_len;
        record_aligned = FUSE_DIRENT_ALIGN(record_size);
        if (record_aligned > chain->writable[1].len - offset)
            break;

        full_path = malloc(dir_len + 1 + name_len + 1);
        if (!full_path)
            continue;
        memcpy(full_path, handle->path, dir_len);
        full_path[dir_len] = '/';
        memcpy(full_path + dir_len + 1, entry->d_name, name_len);
        full_path[dir_len + 1 + name_len] = '\0';

        if (stat(full_path, &st) < 0) {
            free(full_path);
            continue;
        }

        direntplus = (struct fuse_direntplus *) (payload + offset);
        memset(direntplus, 0, record_aligned);
        direntplus->entry_out.nodeid = st.st_ino;
        virtio_fs_fill_attr(&direntplus->entry_out.attr, &st);
        direntplus->dirent.ino = st.st_ino;
        direntplus->dirent.namelen = name_len;
        direntplus->dirent.type = S_ISDIR(st.st_mode) ? 4 : 8;
        memcpy(direntplus->dirent.name, entry->d_name, name_len);
        offset += record_aligned;
        free(full_path);
    }

    if (!virtio_fs_write_iov(vfs, &chain->writable[1], payload, offset) ||
        !virtio_fs_write_response_header(vfs, &chain->writable[0],
                                         req->in.unique, 0, (uint32_t) offset,
                                         used_len)) {
        free(payload);
        return -EFAULT;
    }

    free(payload);
    return 0;
}

static int virtio_fs_process_releasedir(virtio_fs_state_t *vfs,
                                        const struct virtq_chain *chain,
                                        const struct vfs_req_header *req,
                                        uint32_t *used_len)
{
    struct fuse_release_in release_in;
    virtio_fs_handle_entry *handle;

    if (!vfs || !chain || !req || !used_len || chain->readable_count < 2 ||
        chain->writable_count < 1)
        return -EINVAL;
    if (!virtio_fs_read_iov(vfs, &chain->readable[1], &release_in,
                            sizeof(release_in)))
        return -EFAULT;

    handle = virtio_fs_remove_handle(vfs, release_in.fh, VIRTIO_FS_HANDLE_DIR);
    if (!handle)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -EBADF, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    virtio_fs_destroy_handle(handle);
    return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                           req->in.unique, 0, 0, used_len)
               ? 0
               : -EFAULT;
}

static int virtio_fs_process_lookup(virtio_fs_state_t *vfs,
                                    const struct virtq_chain *chain,
                                    const struct vfs_req_header *req,
                                    uint32_t *used_len)
{
    char *name_buf;
    const char *parent_path;
    char *host_path;
    size_t host_path_len;
    struct stat st;
    struct fuse_entry_out entry_out = {0};
    inode_map_entry *entry;

    if (!vfs || !chain || !req || !used_len || chain->readable_count < 2 ||
        chain->writable_count < 2 || chain->readable[1].len == 0)
        return -EINVAL;

    name_buf = calloc(1, chain->readable[1].len + 1);
    if (!name_buf)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOMEM, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    if (!virtio_fs_read_iov(vfs, &chain->readable[1], name_buf,
                            chain->readable[1].len)) {
        free(name_buf);
        return -EFAULT;
    }
    if (!virtio_fs_lookup_name_valid(name_buf, chain->readable[1].len)) {
        free(name_buf);
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -EINVAL, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    }

    parent_path = virtio_fs_path_for_inode(vfs, req->in.nodeid);
    if (!parent_path) {
        free(name_buf);
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOENT, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    }

    host_path_len = strlen(parent_path) + 1 + strlen(name_buf) + 1;
    host_path = malloc(host_path_len);
    if (!host_path) {
        free(name_buf);
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOMEM, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    }
    snprintf(host_path, host_path_len, "%s/%s", parent_path, name_buf);
    free(name_buf);

    if (stat(host_path, &st) < 0) {
        free(host_path);
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOENT, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    }

    entry = find_inode_path(vfs->inode_map, st.st_ino);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            free(host_path);
            return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                                   req->in.unique, -ENOMEM, 0,
                                                   used_len)
                       ? 0
                       : -EFAULT;
        }
        entry->ino = st.st_ino;
        entry->path = virtio_fs_strdup(host_path);
        if (!entry->path) {
            free(entry);
            free(host_path);
            return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                                   req->in.unique, -ENOMEM, 0,
                                                   used_len)
                       ? 0
                       : -EFAULT;
        }
        entry->next = vfs->inode_map;
        vfs->inode_map = entry;
    }
    free(host_path);

    entry_out.nodeid = st.st_ino;
    virtio_fs_fill_attr(&entry_out.attr, &st);
    if (!virtio_fs_write_iov(vfs, &chain->writable[1], &entry_out,
                             sizeof(entry_out)) ||
        !virtio_fs_write_response_header(vfs, &chain->writable[0],
                                         req->in.unique, 0, sizeof(entry_out),
                                         used_len))
        return -EFAULT;
    return 0;
}

static int virtio_fs_process_open(virtio_fs_state_t *vfs,
                                  const struct virtq_chain *chain,
                                  const struct vfs_req_header *req,
                                  uint32_t *used_len)
{
    struct fuse_open_out open_out = {0};
    const char *target_path;
    uint64_t handle_id;
    int fd;
    int ret;

    if (!vfs || !chain || !req || !used_len || chain->writable_count < 2)
        return -EINVAL;

    target_path = virtio_fs_path_for_inode(vfs, req->in.nodeid);
    if (!target_path)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOENT, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    fd = open(target_path, O_RDONLY);
    if (fd < 0)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -errno, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    ret = virtio_fs_add_file_handle(vfs, fd, &handle_id);
    if (ret < 0) {
        close(fd);
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, ret, 0, used_len)
                   ? 0
                   : -EFAULT;
    }

    open_out.fh = handle_id;
    if (!virtio_fs_write_iov(vfs, &chain->writable[1], &open_out,
                             sizeof(open_out)) ||
        !virtio_fs_write_response_header(vfs, &chain->writable[0],
                                         req->in.unique, 0, sizeof(open_out),
                                         used_len)) {
        virtio_fs_handle_entry *handle =
            virtio_fs_remove_handle(vfs, handle_id, VIRTIO_FS_HANDLE_FILE);
        virtio_fs_destroy_handle(handle);
        return -EFAULT;
    }
    return 0;
}

static int virtio_fs_process_read(virtio_fs_state_t *vfs,
                                  const struct virtq_chain *chain,
                                  const struct vfs_req_header *req,
                                  uint32_t *used_len)
{
    struct fuse_read_in read_in;
    virtio_fs_handle_entry *handle;
    char *buf;
    ssize_t n;

    if (!vfs || !chain || !req || !used_len || chain->readable_count < 2 ||
        chain->writable_count < 2)
        return -EINVAL;
    if (!virtio_fs_read_iov(vfs, &chain->readable[1], &read_in,
                            sizeof(read_in)))
        return -EFAULT;

    handle = virtio_fs_find_handle(vfs, read_in.fh, VIRTIO_FS_HANDLE_FILE);
    if (!handle)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -EBADF, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    buf = malloc(read_in.size ? read_in.size : 1);
    if (!buf)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -ENOMEM, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;

    n = pread(handle->fd, buf, read_in.size, read_in.offset);
    if (n < 0) {
        int err = errno;
        free(buf);
        return virtio_fs_write_response_header(
                   vfs, &chain->writable[0], req->in.unique, -err, 0, used_len)
                   ? 0
                   : -EFAULT;
    }
    if ((guest_size_t) n > chain->writable[1].len) {
        free(buf);
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -EOVERFLOW, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    }

    if (!virtio_fs_write_iov(vfs, &chain->writable[1], buf, (guest_size_t) n) ||
        !virtio_fs_write_response_header(vfs, &chain->writable[0],
                                         req->in.unique, 0, (uint32_t) n,
                                         used_len)) {
        free(buf);
        return -EFAULT;
    }
    free(buf);
    return 0;
}

static int virtio_fs_process_release(virtio_fs_state_t *vfs,
                                     const struct virtq_chain *chain,
                                     const struct vfs_req_header *req,
                                     uint32_t *used_len)
{
    struct fuse_release_in release_in;
    virtio_fs_handle_entry *handle;

    if (!vfs || !chain || !req || !used_len || chain->readable_count < 2 ||
        chain->writable_count < 1)
        return -EINVAL;
    if (!virtio_fs_read_iov(vfs, &chain->readable[1], &release_in,
                            sizeof(release_in)))
        return -EFAULT;

    handle = virtio_fs_remove_handle(vfs, release_in.fh, VIRTIO_FS_HANDLE_FILE);
    if (!handle)
        return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                               req->in.unique, -EBADF, 0,
                                               used_len)
                   ? 0
                   : -EFAULT;
    virtio_fs_destroy_handle(handle);
    return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                           req->in.unique, 0, 0, used_len)
               ? 0
               : -EFAULT;
}

static int virtio_fs_process_simple_ok(virtio_fs_state_t *vfs,
                                       const struct virtq_chain *chain,
                                       const struct vfs_req_header *req,
                                       uint32_t *used_len)
{
    if (!vfs || !chain || !req || !used_len || chain->writable_count < 1)
        return -EINVAL;
    return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                           req->in.unique, 0, 0, used_len)
               ? 0
               : -EFAULT;
}

static int virtio_fs_process_unsupported(virtio_fs_state_t *vfs,
                                         const struct virtq_chain *chain,
                                         const struct vfs_req_header *req,
                                         uint32_t *used_len)
{
    if (!vfs || !chain || !req || !used_len || chain->writable_count < 1)
        return -EINVAL;
    return virtio_fs_write_response_header(vfs, &chain->writable[0],
                                           req->in.unique, -EOPNOTSUPP, 0,
                                           used_len)
               ? 0
               : -EFAULT;
}

static int virtio_fs_process_chain_common(virtio_fs_state_t *vfs,
                                          const struct virtq_chain *chain,
                                          uint32_t *used_len)
{
    struct vfs_req_header req;

    if (!vfs || !chain || !used_len)
        return -EINVAL;
    *used_len = 0;

    if (chain->readable_count == 0 ||
        chain->readable[0].len < sizeof(struct vfs_req_header))
        return -EINVAL;
    if (!ram_dma_read(vfs->common.dma, chain->readable[0].addr, &req,
                      sizeof(req)))
        return -EFAULT;

    switch (req.in.opcode) {
    case FUSE_INIT:
        return virtio_fs_process_fuse_init(vfs, chain, &req, used_len);
    case FUSE_GETATTR:
        return virtio_fs_process_getattr(vfs, chain, &req, used_len);
    case FUSE_OPENDIR:
        return virtio_fs_process_opendir(vfs, chain, &req, used_len);
    case FUSE_READDIRPLUS:
        return virtio_fs_process_readdirplus(vfs, chain, &req, used_len);
    case FUSE_LOOKUP:
        return virtio_fs_process_lookup(vfs, chain, &req, used_len);
    case FUSE_FORGET:
    case FUSE_FLUSH:
    case FUSE_DESTROY:
        return virtio_fs_process_simple_ok(vfs, chain, &req, used_len);
    case FUSE_RELEASEDIR:
        return virtio_fs_process_releasedir(vfs, chain, &req, used_len);
    case FUSE_OPEN:
        return virtio_fs_process_open(vfs, chain, &req, used_len);
    case FUSE_READ:
        return virtio_fs_process_read(vfs, chain, &req, used_len);
    case FUSE_RELEASE:
        return virtio_fs_process_release(vfs, chain, &req, used_len);
    default:
        return virtio_fs_process_unsupported(vfs, chain, &req, used_len);
    }
}

static void virtio_fs_set_fail(virtio_fs_state_t *vfs);

static bool virtio_fs_actor_generation_current(virtio_fs_state_t *vfs,
                                               struct virtio_actor *actor,
                                               uint64_t generation)
{
    (void) vfs;
    return actor && virtio_actor_generation(actor) == generation;
}

static bool virtio_fs_queue_ready_for_actor(virtio_fs_state_t *vfs,
                                            const struct virtq *queue)
{
    unsigned status;

    if (!vfs || !queue || !queue->ready)
        return false;

    status = virtio_fs_status_load(vfs);
    return !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET) &&
           (status & VIRTIO_STATUS__DRIVER_OK);
}

static bool virtio_fs_common_generation_current(virtio_fs_state_t *vfs,
                                                uint64_t generation)
{
    bool current;

    if (!vfs || !vfs->common.initialized)
        return false;

    pthread_mutex_lock(&vfs->common.transport_lock);
    current =
        vfs->common.generation == generation && !vfs->common.reset_in_progress;
    pthread_mutex_unlock(&vfs->common.transport_lock);
    return current;
}

static bool virtio_fs_capture_common_generation(virtio_fs_state_t *vfs,
                                                uint64_t *generation)
{
    unsigned status;
    bool current;

    if (!vfs || !vfs->common.initialized || !generation)
        return false;

    pthread_mutex_lock(&vfs->common.transport_lock);
    status = virtio_fs_status_load(vfs);
    current = !vfs->common.reset_in_progress &&
              (status & VIRTIO_STATUS__DRIVER_OK) &&
              !(status & VIRTIO_STATUS__DEVICE_NEEDS_RESET);
    if (current)
        *generation = vfs->common.generation;
    pthread_mutex_unlock(&vfs->common.transport_lock);
    return current;
}

static bool virtio_fs_begin_actor_completion(virtio_fs_state_t *vfs,
                                             uint64_t actor_generation,
                                             uint64_t common_generation)
{
    bool common_current;

    if (!vfs || !vfs->actor_initialized || !vfs->common.initialized)
        return false;

    pthread_mutex_lock(&vfs->common.transport_lock);
    common_current = vfs->common.generation == common_generation &&
                     !vfs->common.reset_in_progress;
    if (!common_current) {
        pthread_mutex_unlock(&vfs->common.transport_lock);
        return false;
    }

    if (!virtio_actor_begin_completion(&vfs->actor, actor_generation)) {
        pthread_mutex_unlock(&vfs->common.transport_lock);
        return false;
    }
    return true;
}

static void virtio_fs_end_actor_completion(virtio_fs_state_t *vfs)
{
    if (!vfs || !vfs->actor_initialized)
        return;

    virtio_actor_end_completion(&vfs->actor);
    pthread_mutex_unlock(&vfs->common.transport_lock);
}

static void virtio_fs_set_fail_for_actor(virtio_fs_state_t *vfs,
                                         uint64_t actor_generation,
                                         uint64_t common_generation)
{
    if (!virtio_fs_begin_actor_completion(vfs, actor_generation,
                                          common_generation))
        return;
    virtio_fs_set_fail(vfs);
    virtio_fs_end_actor_completion(vfs);
}

static int virtio_fs_actor_drain_queue(void *opaque,
                                       struct virtio_actor *actor,
                                       uint16_t queue_index,
                                       uint64_t generation)
{
    virtio_fs_state_t *vfs = opaque;
    struct virtq *queue;
    bool consumed = false;
    uint64_t common_generation = 0;

    if (!vfs || queue_index >= VFS_NUM_QUEUES) {
        if (vfs)
            virtio_fs_set_fail(vfs);
        return 0;
    }

    queue = &vfs->common.queues[queue_index];

    if (!virtio_fs_actor_generation_current(vfs, actor, generation))
        return 0;
    if (!virtio_fs_queue_ready_for_actor(vfs, queue))
        return 0;
    if (!virtio_fs_capture_common_generation(vfs, &common_generation))
        return 0;

    for (;;) {
        struct virtq_iov readable[VFS_MAX_CHAIN_IOVS];
        struct virtq_iov writable[VFS_MAX_CHAIN_IOVS];
        struct virtq_chain chain = {
            .readable = readable,
            .readable_capacity = ARRAY_SIZE(readable),
            .writable = writable,
            .writable_capacity = ARRAY_SIZE(writable),
        };
        uint16_t available = 0;
        uint32_t used_len = 0;
        int ret;

        if (!virtio_fs_actor_generation_current(vfs, actor, generation))
            return 0;
        if (!virtio_fs_common_generation_current(vfs, common_generation))
            return 0;
        ret = virtio_fs_queue_available(vfs, queue, &available);
        if (ret < 0) {
            virtio_fs_set_fail_for_actor(vfs, generation, common_generation);
            return 0;
        }
        if (available == 0)
            break;

        ret = virtq_pop(vfs->common.dma, queue, &chain);
        if (!virtio_fs_actor_generation_current(vfs, actor, generation))
            return 0;
        if (!virtio_fs_common_generation_current(vfs, common_generation))
            return 0;
        if (ret < 0) {
            virtio_fs_set_fail_for_actor(vfs, generation, common_generation);
            return 0;
        }
        if (ret == 0)
            break;

        ret = virtio_fs_process_chain_common(vfs, &chain, &used_len);
        if (!virtio_fs_actor_generation_current(vfs, actor, generation))
            return 0;
        if (!virtio_fs_common_generation_current(vfs, common_generation))
            return 0;
        if (ret < 0) {
            virtio_fs_set_fail_for_actor(vfs, generation, common_generation);
            return 0;
        }

        if (!virtio_fs_begin_actor_completion(vfs, generation,
                                              common_generation))
            return 0;
        ret = virtq_add_used(vfs->common.dma, queue, chain.head, used_len);
        if (ret < 0) {
            virtio_fs_set_fail(vfs);
            virtio_fs_end_actor_completion(vfs);
            return 0;
        }
        virtio_fs_end_actor_completion(vfs);
        consumed = true;
    }

    if (consumed &&
        virtio_fs_begin_actor_completion(vfs, generation, common_generation)) {
        if (!virtq_interrupt_suppressed(vfs->common.dma, queue))
            virtio_irq_trigger(&vfs->common.irq, VIRTIO_INT__USED_RING);
        virtio_fs_end_actor_completion(vfs);
    }
    return 0;
}

static bool virtio_fs_actor_queue_has_work(void *opaque,
                                           struct virtio_actor *actor,
                                           uint16_t queue_index,
                                           uint64_t generation)
{
    virtio_fs_state_t *vfs = opaque;
    uint16_t available = 0;
    uint64_t common_generation = 0;
    int ret;

    if (!vfs || queue_index >= VFS_NUM_QUEUES)
        return false;
    if (!virtio_fs_actor_generation_current(vfs, actor, generation))
        return false;
    if (!virtio_fs_queue_ready_for_actor(vfs, &vfs->common.queues[queue_index]))
        return false;
    if (!virtio_fs_capture_common_generation(vfs, &common_generation))
        return false;

    ret = virtio_fs_queue_available(vfs, &vfs->common.queues[queue_index],
                                    &available);
    if (ret < 0) {
        virtio_fs_set_fail_for_actor(vfs, generation, common_generation);
        return false;
    }
    return available != 0;
}

static void virtio_fs_actor_failed(void *opaque,
                                   struct virtio_actor *actor UNUSED)
{
    virtio_fs_state_t *vfs = opaque;

    if (vfs)
        virtio_fs_set_fail(vfs);
}

static const struct virtio_actor_ops virtio_fs_actor_ops = {
    .drain_queue = virtio_fs_actor_drain_queue,
    .queue_has_work = virtio_fs_actor_queue_has_work,
    .on_failed = virtio_fs_actor_failed,
};

static int virtio_fs_activate(void *opaque,
                              const struct virtio_activation_context *ctx)
{
    virtio_fs_state_t *vfs = opaque;
    int ret;

    (void) ctx;

    if (!vfs || !vfs->actor_initialized)
        return -EINVAL;

    ret = virtio_actor_start(&vfs->actor);
    if (ret < 0 && ret != -EALREADY)
        return ret;

    ret = virtio_actor_enter_configuring(&vfs->actor);
    if (ret < 0)
        return ret;
    return virtio_actor_activate(&vfs->actor);
}

static int virtio_fs_prepare_reset(void *opaque,
                                   uint64_t old_generation,
                                   uint64_t new_generation)
{
    virtio_fs_state_t *vfs = opaque;
    int ret = 0;

    (void) old_generation;
    (void) new_generation;

    if (vfs && vfs->actor_initialized)
        ret = virtio_actor_reset(&vfs->actor);
    if (ret < 0)
        return ret;

    virtio_fs_close_all_handles(vfs);
    return 0;
}

static int virtio_fs_reset(void *opaque,
                           uint64_t old_generation,
                           uint64_t new_generation)
{
    (void) opaque;
    (void) old_generation;
    (void) new_generation;
    return 0;
}

static int virtio_fs_notify_queue(void *opaque,
                                  uint16_t queue_index,
                                  uint64_t generation)
{
    virtio_fs_state_t *vfs = opaque;
    int ret;

    (void) generation;

    if (!vfs || queue_index >= VFS_NUM_QUEUES) {
        if (vfs)
            virtio_fs_set_fail(vfs);
        return -EINVAL;
    }

    ret = virtio_actor_notify_queue(&vfs->actor, queue_index);
    if (ret == 0 || ret == -EAGAIN)
        return 0;

    virtio_fs_set_fail(vfs);
    return ret;
}

static const struct virtio_device_ops virtio_fs_ops = {
    .activate = virtio_fs_activate,
    .prepare_reset = virtio_fs_prepare_reset,
    .reset = virtio_fs_reset,
    .notify_queue = virtio_fs_notify_queue,
    .read_config = virtio_fs_read_config,
};

static void virtio_fs_set_fail(virtio_fs_state_t *vfs)
{
    unsigned status;

    if (!vfs)
        return;

    status = virtio_fs_status_load(vfs);
    if (vfs->common.initialized)
        virtio_device_common_set_needs_reset(&vfs->common);
    else
        vfs->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;

    if (status & VIRTIO_STATUS__DRIVER_OK) {
        if (vfs->common.initialized)
            virtio_irq_trigger(&vfs->common.irq, VIRTIO_INT__CONF_CHANGE);
        else
            vfs->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
    }
}

static inline uint32_t vfs_preprocess(virtio_fs_state_t *vfs, uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_fs_set_fail(vfs), 0;

    return addr >> 2;
}

static void virtio_fs_update_status(virtio_fs_state_t *vfs, uint32_t status)
{
    vfs->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vfs->ram;
    void *priv = vfs->priv;
    char *mount_tag = vfs->mount_tag;
    if (!vfs->shared_dir)
        return;
    size_t shared_dir_len = strlen(vfs->shared_dir) + 1;
    char *shared_dir = (char *) malloc(shared_dir_len);
    if (!shared_dir) {
        shared_dir = NULL;
    } else {
        snprintf(shared_dir, shared_dir_len, "%s", vfs->shared_dir);
    }

    inode_map_entry *inode_map = vfs->inode_map;
    memset(vfs, 0, sizeof(*vfs));
    vfs->ram = ram;
    vfs->priv = priv;
    vfs->mount_tag = mount_tag;

    if (shared_dir) {
        vfs->shared_dir = virtio_fs_strdup(shared_dir);
        free(shared_dir);
    } else {
        if (vfs->shared_dir) {
            vfs->shared_dir[0] = '\0';
        }
    }

    vfs->inode_map = inode_map;
}

static void virtio_fs_init_handler(virtio_fs_state_t *vfs,
                                   struct virtq_desc vq_desc[4],
                                   uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    struct fuse_init_out *init_out =
        (struct fuse_init_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);

    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out);
    header_resp->out.error = 0;

    /* Fill init_out with capabilities */
    init_out->major = 7;
    init_out->minor = 41;
    init_out->max_readahead = 0x10000;
    init_out->flags = FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_DO_READDIRPLUS;
    init_out->max_background = 64;
    init_out->congestion_threshold = 32;
    init_out->max_write = 0x131072;
    init_out->time_gran = 1;

    *plen = header_resp->out.len;
}

static void virtio_fs_getattr_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    const struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t inode = in_header->nodeid;
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    struct fuse_attr_out *outattr =
        (struct fuse_attr_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);

    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out);
    header_resp->out.error = 0;

    const char *target_path = NULL;
    struct stat st;

    /* root entry (inode=1) */
    if (inode == 1) {
        target_path = vfs->shared_dir;
    } else {
        inode_map_entry *entry = find_inode_path(vfs->inode_map, inode);
        if (!entry) {
            header_resp->out.error = -ENOENT;
            *plen = sizeof(struct fuse_out_header);
            return;
        }
        target_path = entry->path;
    }

    if (stat(target_path, &st) < 0) {
        header_resp->out.error = -errno;
        *plen = sizeof(struct fuse_out_header);
        return;
    }

    outattr->attr_valid = 60;
    outattr->attr_valid_nsec = 0;
    outattr->attr.ino = st.st_ino;
    outattr->attr.size = st.st_size;
    outattr->attr.blocks = st.st_blocks;
    outattr->attr.atime = st.st_atime;
    outattr->attr.mtime = st.st_mtime;
    outattr->attr.ctime = st.st_ctime;
    outattr->attr.mode = st.st_mode;
    outattr->attr.nlink = st.st_nlink;
    outattr->attr.uid = st.st_uid;
    outattr->attr.gid = st.st_gid;
    outattr->attr.blksize = st.st_blksize;

    *plen = header_resp->out.len;
}

static void virtio_fs_opendir_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t nodeid = in_header->nodeid;

    inode_map_entry *entry = find_inode_path(vfs->inode_map, nodeid);
    if (!entry) {
        return;
    }

    DIR *dir = opendir(entry->path);
    if (!dir) {
        return;
    }

    /* Allocate dir_handle_t structure */
    dir_handle_t *handle = malloc(sizeof(dir_handle_t));
    if (!handle) {
        closedir(dir);
        return;
    }
    handle->dir = dir;

    /* Dynamically allocate and copy the path string */
    size_t path_len = strlen(entry->path) + 1;
    handle->path = malloc(path_len);
    if (!handle->path) {
        closedir(dir);
        free(handle);
        return;
    }
    memcpy(handle->path, entry->path, path_len);

    struct fuse_open_out *open_out =
        (struct fuse_open_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);
    memset(open_out, 0, sizeof(*open_out));
    open_out->fh = (uint64_t) handle;
    open_out->open_flags = 0;

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_readdirplus_handler(virtio_fs_state_t *vfs,
                                          struct virtq_desc vq_desc[4],
                                          uint32_t *plen)
{
    struct fuse_read_in *read_in =
        (struct fuse_read_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    dir_handle_t *handle = (dir_handle_t *) (uintptr_t) read_in->fh;
    if (!handle || !handle->dir) {
        return;
    }

    DIR *dir = handle->dir;
    const char *dir_path = handle->path;

    uintptr_t base = (uintptr_t) vfs->ram + vq_desc[3].addr;
    size_t offset = 0;

    rewinddir(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        size_t dir_len = strlen(dir_path);
        size_t name_len = strlen(entry->d_name);
        size_t full_len = dir_len + 1 + name_len + 1; /* '/' + name + '\0' */

        /* Dynamically allocate buffer for full_path */
        char *full_path = (char *) malloc(full_len);
        if (!full_path) {
            fprintf(stderr, "malloc failed for full_path\n");
            continue;
        }

        /* Build the full path */
        memcpy(full_path, dir_path, dir_len);
        full_path[dir_len] = '/';
        memcpy(full_path + dir_len + 1, entry->d_name, name_len);
        full_path[dir_len + 1 + name_len] = '\0';

        struct stat st;
        if (stat(full_path, &st) < 0) {
            printf("[READDIRPLUS] stat failed for: %s\n", full_path);
            free(full_path);
            continue;
        }

        struct fuse_entry_out *entry_out =
            (struct fuse_entry_out *) (base + offset);
        memset(entry_out, 0, sizeof(*entry_out));
        entry_out->nodeid = st.st_ino;
        entry_out->attr.ino = st.st_ino;
        entry_out->attr.mode = st.st_mode;
        entry_out->attr.nlink = st.st_nlink;
        entry_out->attr.size = st.st_size;
        entry_out->attr.atime = st.st_atime;
        entry_out->attr.mtime = st.st_mtime;
        entry_out->attr.ctime = st.st_ctime;
        entry_out->attr.uid = st.st_uid;
        entry_out->attr.gid = st.st_gid;
        entry_out->attr.blksize = st.st_blksize;
        entry_out->attr.blocks = st.st_blocks;

        struct fuse_direntplus *direntplus =
            (struct fuse_direntplus *) (base + offset +
                                        sizeof(struct fuse_entry_out));
        direntplus->dirent.ino = st.st_ino;
        direntplus->dirent.namelen = name_len;
        direntplus->dirent.type = S_ISDIR(st.st_mode) ? 4 : 8;
        memcpy(direntplus->dirent.name, entry->d_name, name_len);

        size_t dirent_size = sizeof(struct fuse_direntplus) + name_len;
        size_t dirent_aligned = (dirent_size + 7) & ~7;
        offset += sizeof(struct fuse_entry_out) + dirent_aligned;

        free(full_path);
    }

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header) + offset;
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
    if (header_resp->out.error)
        printf("[READDIRPLUS] error: %d\n", header_resp->out.error);
}

static void virtio_fs_releasedir_handler(virtio_fs_state_t *vfs,
                                         struct virtq_desc vq_desc[4],
                                         uint32_t *plen)
{
    struct fuse_release_in *release_in =
        (struct fuse_release_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    dir_handle_t *handle = (dir_handle_t *) (uintptr_t) release_in->fh;
    if (handle) {
        if (handle->dir)
            closedir(handle->dir);
        if (handle->path)
            free(handle->path);
        free(handle);
    }
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_lookup_handler(virtio_fs_state_t *vfs,
                                     struct virtq_desc vq_desc[4],
                                     uint32_t *plen)
{
    const struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t parent_inode = in_header->nodeid;

    struct fuse_lookup_in *lookup_in =
        (struct fuse_lookup_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    char *name = (char *) (lookup_in);
    size_t name_len = vq_desc[1].len;

    if (name_len == 0) {
        return;
    }
    char *name_buf = malloc(name_len + 1);
    if (!name_buf) {
        fprintf(stderr, "malloc failed for name_buf\n");
        return;
    }
    memcpy(name_buf, name, name_len);
    name_buf[name_len] = '\0';

    inode_map_entry *parent_entry =
        find_inode_path(vfs->inode_map, parent_inode);
    if (!parent_entry) {
        free(name_buf);
        return;
    }
    const char *parent_path = parent_entry->path;

    size_t parent_len = strlen(parent_path);
    size_t name_len1 = strlen(name_buf);
    size_t host_path_len = parent_len + 1 + name_len1 + 1;

    char *host_path = malloc(host_path_len);
    if (!host_path) {
        fprintf(stderr, "malloc failed for host_path\n");
        free(name_buf);
        return;
    }
    memcpy(host_path, parent_path, parent_len);
    host_path[parent_len] = '/';
    memcpy(host_path + parent_len + 1, name_buf, name_len1);
    host_path[parent_len + 1 + name_len1] = '\0';

    struct stat st;
    if (stat(host_path, &st) < 0) {
        free(name_buf);
        free(host_path);
        struct vfs_resp_header *header_resp =
            (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
        header_resp->out.error = -ENOENT;
        *plen = sizeof(struct fuse_out_header);
        return;
    }

    inode_map_entry *entry = find_inode_path(vfs->inode_map, st.st_ino);
    if (!entry) {
        entry = malloc(sizeof(inode_map_entry));
        if (!entry) {
            free(name_buf);
            free(host_path);
            fprintf(stderr, "malloc failed for inode_map_entry\n");
            return;
        }
        entry->ino = st.st_ino;
        entry->path = virtio_fs_strdup(host_path);
        if (!entry->path) {
            free(entry);
            free(name_buf);
            free(host_path);
            fprintf(stderr, "strdup failed for entry->path\n");
            return;
        }
        entry->next = vfs->inode_map;
        vfs->inode_map = entry;
    }

    free(name_buf);
    free(host_path);

    struct fuse_entry_out *entry_out =
        (struct fuse_entry_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);
    memset(entry_out, 0, sizeof(*entry_out));
    entry_out->nodeid = st.st_ino;
    entry_out->attr.ino = st.st_ino;
    entry_out->attr.mode = st.st_mode;
    entry_out->attr.nlink = st.st_nlink;
    entry_out->attr.size = st.st_size;
    entry_out->attr.atime = st.st_atime;
    entry_out->attr.mtime = st.st_mtime;
    entry_out->attr.ctime = st.st_ctime;
    entry_out->attr.uid = st.st_uid;
    entry_out->attr.gid = st.st_gid;
    entry_out->attr.blksize = st.st_blksize;
    entry_out->attr.blocks = st.st_blocks;

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_open_handler(virtio_fs_state_t *vfs,
                                   struct virtq_desc vq_desc[4],
                                   uint32_t *plen)
{
    const struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t inode = in_header->nodeid;

    const char *target_path = NULL;
    if (inode == 1) {
        target_path = vfs->shared_dir;
    } else {
        inode_map_entry *entry = find_inode_path(vfs->inode_map, inode);
        if (!entry) {
            struct vfs_resp_header *header_resp =
                (struct vfs_resp_header *) ((uintptr_t) vfs->ram +
                                            vq_desc[2].addr);
            header_resp->out.error = -ENOENT;
            *plen = sizeof(struct fuse_out_header);
            return;
        }
        target_path = entry->path;
    }

    int fd = open(target_path, O_RDONLY);
    if (fd < 0) {
        struct vfs_resp_header *header_resp =
            (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
        header_resp->out.error = -errno;
        *plen = sizeof(struct fuse_out_header);
        fprintf(stderr, "[OPEN] failed: %s, error=%s\n", target_path,
                strerror(errno));
        return;
    }

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    struct fuse_open_out *open_out =
        (struct fuse_open_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);

    open_out->fh = (uint64_t) fd;
    open_out->open_flags = 0;
    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_read_handler(virtio_fs_state_t *vfs,
                                   struct virtq_desc vq_desc[4],
                                   uint32_t *plen)
{
    struct fuse_read_in *read_in =
        (struct fuse_read_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    int fd = (int) (uintptr_t) read_in->fh;
    off_t offset = read_in->offset;
    size_t size = read_in->size;

    char *buf = malloc(size);
    if (!buf) {
        struct vfs_resp_header *header_resp =
            (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
        header_resp->out.error = -ENOMEM;
        *plen = sizeof(struct fuse_out_header);
        fprintf(stderr, "[READ] malloc failed, size=%zu\n", size);
        return;
    }

    ssize_t n = pread(fd, buf, size, offset);
    if (n < 0) {
        struct vfs_resp_header *header_resp =
            (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
        header_resp->out.error = -errno;
        *plen = sizeof(struct fuse_out_header);
        fprintf(stderr, "[READ] failed: fd=%d, errno=%d\n", fd, errno);
        free(buf);
        return;
    }

    memcpy((void *) ((uintptr_t) vfs->ram + vq_desc[3].addr), buf, n);

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header) + n;
    header_resp->out.error = 0;
    *plen = header_resp->out.len;

    free(buf);
}

static void virtio_fs_release_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    struct fuse_release_in *release_in =
        (struct fuse_release_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    int fd = (int) release_in->fh;
    close(fd);
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_flush_handler(virtio_fs_state_t *vfs,
                                    struct virtq_desc vq_desc[4],
                                    uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}


static void virtio_fs_forget_handler(virtio_fs_state_t *vfs,
                                     struct virtq_desc vq_desc[4],
                                     uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_destroy_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_default_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    const struct vfs_req_header *header_req =
        (struct vfs_req_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    struct fuse_out_header out = {
        .len = sizeof(struct fuse_out_header),
        .error = -EOPNOTSUPP,
        .unique = header_req->in.unique,
    };
    /* Copy to output buffer */
    if (vq_desc[2].len >= sizeof(out)) {
        memcpy((void *) ((uintptr_t) vfs->ram + vq_desc[2].addr), &out,
               sizeof(out));
        *plen = sizeof(out);
    } else {
        fprintf(stderr, "output buffer too small for error reply!\n");
        *plen = 0;
    }
}

static int virtio_fs_desc_handler(virtio_fs_state_t *vfs,
                                  const virtio_fs_queue_t *queue,
                                  uint32_t desc_idx,
                                  uint32_t *plen)
{
    struct virtq_desc vq_desc[4];
    for (int i = 0; i < 4; i++) {
        /* The size of the `struct virtq_desc` is 4 words */
        const struct virtq_desc *desc =
            (struct virtq_desc *) &vfs->ram[queue->QueueDesc + desc_idx * 4];
        vq_desc[i].addr = desc->addr;
        vq_desc[i].len = desc->len;
        vq_desc[i].flags = desc->flags;
        desc_idx = desc->next;
    }

    const struct vfs_req_header *header_req =
        (struct vfs_req_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint32_t op = header_req->in.opcode;
    switch (op) {
    case FUSE_INIT:
        virtio_fs_init_handler(vfs, vq_desc, plen);
        break;
    case FUSE_GETATTR:
        virtio_fs_getattr_handler(vfs, vq_desc, plen);
        break;
    case FUSE_OPENDIR:
        virtio_fs_opendir_handler(vfs, vq_desc, plen);
        break;
    case FUSE_READDIRPLUS:
        virtio_fs_readdirplus_handler(vfs, vq_desc, plen);
        break;
    case FUSE_LOOKUP:
        virtio_fs_lookup_handler(vfs, vq_desc, plen);
        break;
    case FUSE_FORGET:
        virtio_fs_forget_handler(vfs, vq_desc, plen);
        break;
    case FUSE_RELEASEDIR:
        virtio_fs_releasedir_handler(vfs, vq_desc, plen);
        break;
    case FUSE_OPEN:
        virtio_fs_open_handler(vfs, vq_desc, plen);
        break;
    case FUSE_READ:
        virtio_fs_read_handler(vfs, vq_desc, plen);
        break;
    case FUSE_RELEASE:
        virtio_fs_release_handler(vfs, vq_desc, plen);
        break;
    case FUSE_FLUSH:
        virtio_fs_flush_handler(vfs, vq_desc, plen);
        break;
    case FUSE_DESTROY:
        virtio_fs_destroy_handler(vfs, vq_desc, plen);
        break;
    default:
        virtio_fs_default_handler(vfs, vq_desc, plen);
        break;
    }
    /* TODO: FUSE_WRITE, FUSE_MKDIR, FUSE_RMDIR, FUSE_CREATE */

    return 0;
}

static void virtio_queue_notify_handler(virtio_fs_state_t *vfs, int index)
{
    uint32_t *ram = vfs->ram;
    virtio_fs_queue_t *queue = &vfs->queues[index];
    if (vfs->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return;

    if (!((vfs->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        return virtio_fs_set_fail(vfs);

    uint16_t new_avail = ram_load_high16_acquire(&ram[queue->QueueAvail]);
    if (new_avail - queue->last_avail > (uint16_t) queue->QueueNum)
        return virtio_fs_set_fail(vfs);

    if (queue->last_avail == new_avail)
        return;

    uint16_t new_used = ram_load_high16(&ram[queue->QueueUsed]);
    while (queue->last_avail != new_avail) {
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;
        uint16_t buffer_idx =
            ram_load_w_acquire(&ram[queue->QueueAvail + 1 + queue_idx / 2]) >>
            (16 * (queue_idx % 2));

        uint32_t len = 0;
        int result = virtio_fs_desc_handler(vfs, queue, buffer_idx, &len);
        if (result != 0)
            return virtio_fs_set_fail(vfs);

        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram_store_w(&ram[vq_used_addr], buffer_idx);
        ram_store_w(&ram[vq_used_addr + 1], len);
        queue->last_avail++;
        new_used++;
    }

    ram_store_high16_release(&vfs->ram[queue->QueueUsed], new_used);

    if (!(ram_load_w_acquire(&ram[queue->QueueAvail]) & 1))
        vfs->InterruptStatus |= VIRTIO_INT__USED_RING;
}

static bool UNUSED virtio_fs_reg_read(virtio_fs_state_t *vfs,
                                      uint32_t addr,
                                      uint32_t *value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(MagicValue):
        *value = 0x74726976; /* "virt" */
        return true;
    case _(Version):
        *value = 2;
        return true;
    case _(DeviceID):
        *value = 26; /* = virtio-fs */
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;
    case _(DeviceFeatures):
        *value = vfs->DeviceFeaturesSel == 0
                     ? VFS_FEATURES_0
                     : (vfs->DeviceFeaturesSel == 1 ? VFS_FEATURES_1 : 0);
        return true;
    case _(QueueNumMax):
        *value = VFS_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VFS_QUEUE.ready ? 1 : 0;
        return true;
    case _(InterruptStatus):
        *value = vfs->InterruptStatus;
        return true;
    case _(Status):
        *value = vfs->Status;
        return true;
    case _(ConfigGeneration):
        *value = 0;
        return true;
    case NUM_REQUEST_QUEUES_ADDR:
        *value = ((uint32_t *) PRIV(vfs))[addr - _(Config)];
        return true;
    default:
        if (!RANGE_CHECK((addr >> 2), _(Config),
                         sizeof(struct virtio_fs_config)))
            return false;
        uint32_t cfg_offset = addr - ((_(Config)) << 2);
        uint8_t *cfg_bytes = (uint8_t *) PRIV(vfs);
        *value = cfg_bytes[cfg_offset];

        return true;
    }
#undef _
}

static bool UNUSED virtio_fs_reg_write(virtio_fs_state_t *vfs,
                                       uint32_t addr,
                                       uint32_t value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vfs->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        if (vfs->DriverFeaturesSel == 0)
            vfs->DriverFeatures = value;
        return true;
    case _(DriverFeaturesSel):
        vfs->DriverFeaturesSel = value;
        return true;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vfs->queues)) {
            vfs->QueueSel = value;
        } else
            virtio_fs_set_fail(vfs);
        return true;
    case _(QueueNum):
        if (value > 0 && value <= VFS_QUEUE_NUM_MAX) {
            VFS_QUEUE.QueueNum = value;
        } else
            virtio_fs_set_fail(vfs);
        return true;
    case _(QueueReady):
        VFS_QUEUE.ready = value & 1;
        if (value & 1)
            VFS_QUEUE.last_avail =
                ram_load_high16_acquire(&vfs->ram[VFS_QUEUE.QueueAvail]);
        return true;
    case _(QueueDescLow):
        VFS_QUEUE.QueueDesc = vfs_preprocess(vfs, value);
        return true;
    case _(QueueDescHigh):
        if (value) {
            virtio_fs_set_fail(vfs);
        }
        return true;
    case _(QueueDriverLow):
        VFS_QUEUE.QueueAvail = vfs_preprocess(vfs, value);
        return true;
    case _(QueueDriverHigh):
        if (value) {
            virtio_fs_set_fail(vfs);
        }
        return true;
    case _(QueueDeviceLow):
        VFS_QUEUE.QueueUsed = vfs_preprocess(vfs, value);
        return true;
    case _(QueueDeviceHigh):
        if (value) {
            virtio_fs_set_fail(vfs);
        }
        return true;
    case _(QueueNotify):
        if (value < ARRAY_SIZE(vfs->queues)) {
            virtio_queue_notify_handler(vfs, value);
        } else
            virtio_fs_set_fail(vfs);
        return true;
    case _(InterruptACK):
        vfs->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_fs_update_status(vfs, value);
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_fs_config)))
            return false;

        /* Write configuration to the corresponding register */
        ((uint32_t *) PRIV(vfs))[addr - _(Config)] = value;

        return true;
    }
#undef _
}

static bool virtio_fs_load_width_bytes(uint8_t width, size_t *access_size)
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

static bool virtio_fs_store_width_bytes(uint8_t width, size_t *access_size)
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

static bool virtio_fs_is_config_access(uint32_t addr, size_t access_size)
{
    const uint32_t base = VIRTIO_Config << 2;
    const uint32_t end = base + (uint32_t) sizeof(struct virtio_fs_config);

    if (access_size == 0 || addr < base || addr >= end)
        return false;
    return access_size <= end - addr;
}

void virtio_fs_read(hart_t *vm,
                    virtio_fs_state_t *vfs,
                    uint32_t addr,
                    uint8_t width,
                    uint32_t *value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_fs_load_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_fs_is_config_access(addr, access_size);
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

    ret = virtio_mmio_read(&vfs->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
}

void virtio_fs_write(hart_t *vm,
                     virtio_fs_state_t *vfs,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t value)
{
    size_t access_size = 0;
    bool is_cfg;
    int ret;

    if (!virtio_fs_store_width_bytes(width, &access_size)) {
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        return;
    }

    is_cfg = virtio_fs_is_config_access(addr, access_size);
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

    ret = virtio_mmio_write(&vfs->common, addr, (uint8_t) access_size, value);
    if (ret < 0)
        vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
}

bool virtio_fs_irq_pending(virtio_fs_state_t *vfs)
{
    return vfs && vfs->common.initialized &&
           virtio_irq_read_status(&vfs->common.irq) != 0;
}

static char *virtio_fs_strdup(const char *src)
{
    size_t len;
    char *dst;

    if (!src)
        return NULL;
    len = strlen(src) + 1;
    dst = malloc(len);
    if (!dst)
        return NULL;
    memcpy(dst, src, len);
    return dst;
}

static void virtio_fs_free_inode_map(inode_map_entry *entry)
{
    while (entry) {
        inode_map_entry *next = entry->next;

        free(entry->path);
        free(entry);
        entry = next;
    }
}

bool virtio_fs_init(virtio_fs_state_t *vfs,
                    emu_state_t *emu,
                    char *mtag,
                    char *dir)
{
    static const uint16_t queue_max_sizes[VFS_NUM_QUEUES] = {
        [0] = VFS_QUEUE_NUM_MAX,
        [1] = VFS_QUEUE_NUM_MAX,
        [2] = VFS_QUEUE_NUM_MAX,
    };
    struct virtio_device_common_config common_config;

    if (!vfs || !emu) {
        fprintf(stderr, "Failed to initialize virtio-fs common device.\n");
        exit(2);
    }

    if (vfs_dev_cnt >= VFS_DEV_CNT_MAX) {
        fprintf(stderr,
                "Exceeded the number of virtio-fs devices that can be "
                "allocated.\n");
        exit(2);
    }

    memset(vfs, 0, sizeof(*vfs));
    vfs->next_handle_id = 1;
    vfs->ram = emu->ram;
    vfs->priv = &vfs_configs[vfs_dev_cnt++];
    memset(PRIV(vfs), 0, sizeof(*PRIV(vfs)));
    snprintf(PRIV(vfs)->tag, sizeof(PRIV(vfs)->tag), "%s", mtag ? mtag : "");
    PRIV(vfs)->num_request_queues = 2;
    vfs->mount_tag = mtag;

    common_config = (struct virtio_device_common_config) {
        .emu = emu,
        .dma = &emu->ram_dma,
        .irq_source = SEMU_IRQ_SOURCE_VFS,
        .device_id = 26,
        .vendor_id = VIRTIO_VENDOR_ID,
        .device_features = VIRTIO_FS_F_VERSION_1,
        .required_features = VIRTIO_FS_F_VERSION_1,
        .queue_max_sizes = queue_max_sizes,
        .num_queues = ARRAY_SIZE(queue_max_sizes),
        .ops = &virtio_fs_ops,
        .opaque = vfs,
    };

    if (virtio_device_common_init(&vfs->common, &common_config) < 0) {
        fprintf(stderr, "Failed to initialize virtio-fs common device.\n");
        exit(2);
    }

    if (virtio_actor_init(&vfs->actor, &virtio_fs_actor_ops, vfs,
                          ARRAY_SIZE(queue_max_sizes)) < 0) {
        virtio_device_common_destroy(&vfs->common);
        fprintf(stderr, "Failed to initialize virtio-fs actor.\n");
        exit(2);
    }
    vfs->actor_initialized = true;

    if (!dir) {
        /* -s parameter is empty; keep the MMIO device initialized but report
         * that no host filesystem share is active.
         */
        return false;
    }

    int dir_fd = open(dir, O_RDONLY);
    if (dir_fd < 0) {
        fprintf(stderr, "Could not open directory: %s\n", dir);
        exit(2);
    }
    close(dir_fd);

    vfs->shared_dir = virtio_fs_strdup(dir);
    if (!vfs->shared_dir) {
        fprintf(stderr, "Failed to allocate memory for shared_dir\n");
        exit(2);
    }

    inode_map_entry *root_entry = malloc(sizeof(inode_map_entry));
    if (!root_entry) {
        fprintf(stderr, "Failed to allocate memory for root_entry\n");
        return false;
    }
    root_entry->ino = 1;
    root_entry->path = virtio_fs_strdup(vfs->shared_dir);
    if (!root_entry->path) {
        fprintf(stderr, "Failed to allocate memory for root_entry->path\n");
        free(root_entry);
        return false;
    }
    root_entry->next = vfs->inode_map;
    vfs->inode_map = root_entry;

    return true;
}

void virtio_fs_destroy(virtio_fs_state_t *vfs)
{
    if (!vfs)
        return;

    if (vfs->actor_initialized) {
        virtio_actor_stop(&vfs->actor);
        virtio_actor_destroy(&vfs->actor);
        vfs->actor_initialized = false;
    }

    virtio_fs_close_all_handles(vfs);
    if (vfs->common.initialized)
        virtio_device_common_destroy(&vfs->common);
    virtio_fs_free_inode_map(vfs->inode_map);
    free(vfs->shared_dir);
    vfs->inode_map = NULL;
    vfs->shared_dir = NULL;
    vfs->mount_tag = NULL;
    vfs->ram = NULL;
    if (vfs->priv && vfs_dev_cnt > 0)
        vfs_dev_cnt--;
    vfs->priv = NULL;
}
