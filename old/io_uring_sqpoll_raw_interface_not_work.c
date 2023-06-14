#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <linux/io_uring.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define ENTRIES 32

struct buf_info {
    off_t offset; // fd offset
    size_t len;   // buffer length
    char *buf;    // buffer
};

struct file_info {
    int fd;                    // file descriptor
    size_t file_size;          // file size
    size_t blocks;             // file blocks
    struct buf_info buffers[]; // buffers
};

int open_file(char *filename) {
    int fd = open(filename, O_RDONLY | O_DIRECT); // | O_DIRECT);
    if (fd < 0) {
        perror("open: ");
        exit(-1);
    }
    return fd;
}

size_t get_file_size(int fd) {
    struct stat stat;
    int ret = fstat(fd, &stat);
    if (ret != 0) {
        perror("fstat: ");
        exit(-ret);
    }
    return stat.st_size;
}

void seen_cqe(struct io_uring *io_uring, struct io_uring_cqe *cqe) {
    struct io_uring_cq *cq = &io_uring->cq;
    if (cqe) {
        atomic_store_explicit((atomic_uint *)cq->khead, *cq->khead + 1, memory_order_release);
    }
}

struct io_uring_cqe *wait_cqe(struct io_uring *io_uring) {
    struct io_uring_cqe *cqe = NULL;
    unsigned available;
    unsigned mask = io_uring->cq.ring_mask;
    int shift = 0;
    if (io_uring->flags & IORING_SETUP_CQE32) {
        shift = 1;
    }
    int err = 0;
    // peek first
    do {
        unsigned tail = atomic_load_explicit((atomic_uint *)io_uring->cq.ktail, memory_order_acquire);
        unsigned head = *io_uring->cq.khead;

        cqe = NULL;
        available = tail - head;
        if (!available)
            break;

        cqe = &io_uring->cq.cqes[(head & mask) << shift];
        if (!(io_uring->features & IORING_FEAT_EXT_ARG) &&
            cqe->user_data == LIBURING_UDATA_TIMEOUT) {
            if (cqe->res < 0)
                err = cqe->res;
            seen_cqe(io_uring, cqe);
            if (!err)
                continue;
            cqe = NULL;
        }

        break;
    } while (1);

    if (available == 0) {
        return NULL;
    }
    // *cqe_ptr = cqe;
    // if (nr_available)
    //     *nr_available = available;

    // wait cqe second
    do {
        unsigned flags = 0;
        if (!cqe) {
            continue;
        }
        if (atomic_load_explicit((atomic_uint *)io_uring->sq.kflags, memory_order_relaxed) & IORING_SQ_NEED_WAKEUP) {
            flags |= IORING_ENTER_SQ_WAKEUP;
            int ret = io_uring_enter(io_uring->ring_fd, 0, 1, flags, NULL);
            if (ret < 0) {
                fprintf(stderr, "io_uring_enter: sq_wakeup: %s\n", strerror(ret));
                cqe = NULL;
                break;
            }
        }
        break;
    } while (1);
    return cqe;
}

struct io_uring_sqe *get_sqe(struct io_uring *io_uring) {
    struct io_uring_sq *sq = &io_uring->sq;
    unsigned head, next = sq->sqe_tail + 1;
    int shift = 0;

    if (io_uring->flags & IORING_SETUP_SQE128) {
        shift = 1;
    }
    // atomic load
    head = atomic_load_explicit((atomic_uint *)sq->khead, memory_order_acquire);

    if (next - head <= sq->ring_entries) {
        struct io_uring_sqe *sqe;
        sqe = &sq->sqes[(sq->sqe_tail & sq->ring_mask) << shift];
        sq->sqe_tail = next;
        return sqe;
    }
    return NULL;
}

int submit_sq(struct io_uring *io_uring) {
    // flush sq
    struct io_uring_sq *sq = &io_uring->sq;
    unsigned tail = sq->sqe_tail;

    if (sq->sqe_head != tail) {
        sq->sqe_head = tail;

        atomic_store_explicit((atomic_uint *)sq->ktail, tail, memory_order_release);
    }
    int flushed = tail - *sq->khead;
    int ret = 0;
    unsigned flags = 0;
    // check enter cq
    if (atomic_load_explicit((atomic_uint *)sq->kflags, memory_order_relaxed) & IORING_SQ_TASKRUN) {
        flags |= IORING_ENTER_GETEVENTS;
    }
    if (flushed && atomic_load_explicit((atomic_uint *)sq->kflags, memory_order_relaxed) & IORING_SQ_NEED_WAKEUP) {
        flags |= IORING_ENTER_SQ_WAKEUP;
    }
    if (flags) {
        ret = io_uring_enter(io_uring->ring_fd, flushed, 0, flags, NULL);
        return ret;
    }
    ret = flushed;
    return ret;
}

int sq_ready(struct io_uring *io_uring) {
    unsigned khead = *io_uring->sq.khead;
    if (io_uring->flags & IORING_SETUP_SQPOLL) {
        khead = atomic_load_explicit((atomic_uint *)io_uring->sq.khead, memory_order_acquire);
    }
    return io_uring->sq.sqe_tail - khead;
}

int sq_space_left(struct io_uring *io_uring) {
    return io_uring->sq.ring_entries - sq_ready(io_uring);
}

int sq_full(struct io_uring *io_uring) {
    return sq_space_left(io_uring) == 0;
}

int cq_ready(struct io_uring *io_uring) {
    return atomic_load_explicit((atomic_uint *)io_uring->cq.ktail, memory_order_acquire) - *io_uring->cq.khead;
}

int cq_space_left(struct io_uring *io_uring) {
    return io_uring->cq.ring_entries - cq_ready(io_uring);
}

int cq_full(struct io_uring *io_uring) {
    return cq_space_left(io_uring) == 0;
}

int check_cqe(struct io_uring *io_uring) {
    struct io_uring_cqe *cqe;
    if (!io_uring_cq_ready(io_uring)) {
        return 1;
    }
    int ret = io_uring_wait_cqe(io_uring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-ret));
        return ret;
    }
    if (cqe->res < 0) {
        fprintf(stderr, "cqe res: %s\n", strerror(-cqe->res));
        return cqe->res;
    }
    io_uring_cqe_seen(io_uring, cqe);
    return 0;
}

int read_block(struct io_uring *io_uring, struct buf_info *buf_info, int fd) {
    struct io_uring_sqe *sqe;

    // while (sq_full(io_uring)) {
    //     // check cq
    //     while (cq_ready(io_uring)) {
    //         struct io_uring_cqe *cqe = wait_cqe(io_uring);
    //         if (!cqe) {
    //             fprintf(stderr, "wait_cqe fail\n");
    //         }
    //         seen_cqe(io_uring, cqe);
    //     }
    // }
    // sqe = get_sqe(io_uring);
    // if (sqe == NULL) {
    //     fprintf(stderr, "get_sqe failed\n");
    //     exit(-1);
    // }
    // // prep read
    // sqe->opcode = (__u8)IORING_OP_READ;
    // sqe->flags = IOSQE_FIXED_FILE;
    // sqe->ioprio = 0;
    // sqe->fd = fd;
    // sqe->off = buf_info->offset;
    // sqe->addr = (unsigned long)buf_info->buf;
    // sqe->len = buf_info->len;
    // sqe->rw_flags = 0;
    // sqe->buf_index = 0;
    // sqe->personality = 0;
    // sqe->file_index = 0;
    // sqe->addr3 = 0;
    // sqe->__pad2[0] = 0;
    // sqe->user_data = (unsigned long)buf_info;
    // int flushed = submit_sq(io_uring);
    // printf("flushed %d\n", flushed);
    while (1) {
        while (!io_uring_sq_space_left(io_uring)) {
            while (io_uring_sq_ready(io_uring)) {
                check_cqe(io_uring);
            }
        }
        struct io_uring_sqe *sqe = io_uring_get_sqe(io_uring);
        if (!sqe) {
            fprintf(stderr, "io_uring_get_sqe failed");
            continue;
        }
        io_uring_prep_read(sqe, fd, buf_info->buf, buf_info->len, buf_info->offset);
        io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
        io_uring_sqe_set_data(sqe, buf_info->offset);
        if (io_uring_sq_space_left(io_uring) == 0) {
            int submitted = io_uring_submit(io_uring);
        }
        break;
    }
}

int read_file(struct io_uring *io_uring, struct file_info *file_info) {
    for (int i = 0; i < file_info->blocks; i++) {
        int buf_index = (i % 2) ? (file_info->blocks - (i / 2) - 1) : i / 2;
        read_block(io_uring, &file_info->buffers[buf_index], 0);
    }
    while (io_uring_sq_ready(io_uring) || io_uring_cq_ready(io_uring)) {
        // struct io_uring_cqe *cqe = wait_cqe(io_uring);
        // if (!cqe) {
        //     fprintf(stderr, "wait_cqe fail\n");
        // }
        // seen_cqe(io_uring, cqe);
        check_cqe(io_uring);
    }
}

int register_file(struct io_uring *io_uring, int fd) {
    return io_uring_register(io_uring->ring_fd, IORING_REGISTER_FILES, &fd, 1);
}

struct file_info *prepare_file(char *filename) {
    int fd = open_file(filename);
    if (fd < 0) {
        return NULL;
    }
    size_t file_size = get_file_size(fd);
    size_t blocks = file_size / BUF_SIZE + (file_size % BUF_SIZE ? 1 : 0);
    struct file_info *file_info = malloc(sizeof(struct file_info) + (sizeof(struct buf_info) * blocks));
    if (!file_info) {
        return NULL;
    }
    memset(file_info, 0, sizeof(struct file_info) + (sizeof(struct buf_info) * blocks));
    file_info->fd = fd;
    file_info->file_size = file_size;
    file_info->blocks = blocks;

    // buffer alloc
    for (int i = 0; i < blocks; i++) {
        int ret = posix_memalign(&file_info->buffers[i].buf, BUF_SIZE, BUF_SIZE);
        if (ret != 0) {
            fprintf(stderr, "posix_memalign: %s\n", strerror(ret));
            return NULL;
        }
        file_info->buffers[i].len = (i == blocks - 1 && (file_size % BUF_SIZE) ? (file_size % BUF_SIZE) : BUF_SIZE);
        file_info->buffers[i].offset = i * BUF_SIZE;
    }

    return file_info;
}

int init_ring(struct io_uring *io_uring, struct io_uring_params *params) {
    int fd;
    unsigned *sq_array;
    unsigned sq_entries, index;
    memset(io_uring, 0, sizeof(struct io_uring));
    fd = io_uring_setup(ENTRIES, params);
    if (fd < 0) {
        fprintf(stderr, "io_uring_setup: %s\n", strerror(-fd));
        return -1;
    }
    io_uring->ring_fd = fd;

    // mmap
    size_t cqe_size = sizeof(struct io_uring_cqe);
    if (params->flags & IORING_SETUP_CQE32) {
        cqe_size += sizeof(struct io_uring_cqe);
    }

    struct io_uring_sq *sq = &io_uring->sq;
    struct io_uring_cq *cq = &io_uring->cq;

    sq->ring_sz = params->sq_off.array + params->sq_entries * sizeof(unsigned);
    cq->ring_sz = params->cq_off.cqes + params->cq_entries * cqe_size;

    if (params->features & IORING_FEAT_SINGLE_MMAP) {
        if (cq->ring_sz > sq->ring_sz) {
            sq->ring_sz = cq->ring_sz;
        }
        cq->ring_sz = sq->ring_sz;
    }

    // sqring mmap
    sq->ring_ptr = mmap(0, sq->ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq->ring_ptr < 0) {
        perror("mmap sqring: ");
        return -1;
    }

    // cqring mmap
    if (params->features & IORING_FEAT_SINGLE_MMAP) {
        cq->ring_ptr = sq->ring_ptr;
    } else {
        cq->ring_ptr = mmap(0, cq->ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        if (cq->ring_ptr < 0) {
            perror("mmap cqring: ");
            return -1;
        }
    }

    // sqring values
    sq->khead = sq->ring_ptr + params->sq_off.head;
    sq->ktail = sq->ring_ptr + params->sq_off.tail;
    sq->kring_mask = sq->ring_ptr + params->sq_off.ring_mask;
    sq->kring_entries = sq->ring_ptr + params->sq_off.ring_entries;
    sq->kflags = sq->ring_ptr + params->sq_off.flags;
    sq->kdropped = sq->ring_ptr + params->sq_off.dropped;
    sq->array = sq->ring_ptr + params->sq_off.array;

    // sqes
    size_t sqe_size = sizeof(struct io_uring_sqe);
    if (params->flags & IORING_SETUP_SQE128) {
        sqe_size += 64;
    }
    sq->sqes = mmap(0, sqe_size * params->sq_entries, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (sq->sqes < 0) {
        perror("mmap sqes: ");
        return -1;
    }

    // cqring values
    cq->khead = cq->ring_ptr + params->cq_off.head;
    cq->ktail = cq->ring_ptr + params->cq_off.tail;
    cq->kring_mask = cq->ring_ptr + params->cq_off.ring_mask;
    cq->kring_entries = cq->ring_ptr + params->cq_off.ring_entries;
    cq->koverflow = cq->ring_ptr + params->cq_off.overflow;
    cq->cqes = cq->ring_ptr + params->cq_off.cqes;
    if (params->cq_off.flags) {
        cq->kflags = cq->ring_ptr + params->cq_off.flags;
    }
    sq->ring_mask = *sq->kring_mask;
    sq->ring_entries = *sq->kring_entries;
    cq->ring_mask = *cq->kring_mask;
    cq->ring_entries = *cq->kring_entries;

    return 0;
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        fprintf(stderr, "usage: %s filename\n", argv[0]);
        return -1;
    }

    struct io_uring io_uring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(struct io_uring_params));
    params.flags = IORING_SETUP_SQPOLL;  // enable sqpoll
    params.flags |= IORING_SETUP_SQ_AFF; // sqpoll cpu affinity
    params.sq_thread_cpu = 1;            // set core 1
    params.sq_thread_idle = 2000;        // idle after 2000ms of inactive

    if (io_uring_queue_init_params(ENTRIES, &io_uring, &params)) {
        fprintf(stderr, "init_ring failed\n");
        return -1;
    }

    // cpu affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset); // main process cpu affinity

    struct file_info *file_info = prepare_file(argv[1]);

    if (register_file(&io_uring, file_info->fd)) {
        fprintf(stderr, "register_file failed\n");
        return -1;
    }

    printf("start read\n");
    read_file(&io_uring, file_info);
    printf("read to buffer done\n");
    io_uring_queue_exit(&io_uring);

    return 0;
}