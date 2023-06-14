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
#define ENTRIES 8

#define barrier() __asm__ __volatile__("" :: \
                                           : "memory")

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

// struct io_uring_sq {
//     unsigned *head;         // current head value
//     unsigned *tail;         // current tail value
//     unsigned *ring_mask;    // ring mask value
//     unsigned *ring_entries; // ring entries count value
//     unsigned *flags;        // ring flags
//     unsigned *array;        // ring array
// };

// struct io_uring_cq {
//     unsigned *head;            // current head value
//     unsigned *tail;            // current tail value
//     unsigned *ring_mask;       // ring mask value
//     unsigned *ring_entries;    // ring entries count value
//     struct io_uring_cqe *cqes; // ceqs array
// };

// struct io_uring {
//     int fd;                    // io_uring fd
//     struct io_uring_sq sq;     // submission ring
//     struct io_uring_sqe *sqes; // seq entries
//     struct io_uring_cq cq;     // conpletion ring
// };

/**
 * 'io_uring_setup', 'io_uring_register' and 'io_uring_enter' is not a part of glibc.
 * add syscall wrapper
 */

// int io_uring_setup(unsigned int entries, struct io_uring_params *p) {
//     return (int)syscall(__NR_io_uring_setup, entries, p);
// }

// int io_uring_enter(unsigned int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags) {
//     return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
// }

// int io_uring_register(unsigned int ring_fd, unsigned int opcode, const void *arg, unsigned int nr_args) {
//     return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
// }

int open_file(char *filename) {
    int fd = open(filename, O_RDONLY); // | O_DIRECT);
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

int prepare_file(char *filename, struct file_info **file_info_p) {
    int fd = open_file(filename);
    if (fd < 0) {
        return -1;
    }
    size_t file_size = get_file_size(fd);
    size_t blocks = file_size / BUF_SIZE + (file_size % BUF_SIZE ? 1 : 0);
    *file_info_p = malloc(sizeof(struct file_info) + (sizeof(struct buf_info) * blocks));
    if (!(*file_info_p)) {
        return -1;
    }
    struct file_info *file_info = *file_info_p;
    memset(file_info, 0, sizeof(struct file_info) + (sizeof(struct buf_info) * blocks));
    file_info->fd = fd;
    file_info->file_size = file_size;
    file_info->blocks = blocks;

    // buffer alloc
    for (int i = 0; i < blocks; i++) {
        int ret = posix_memalign(&file_info->buffers[i].buf, BUF_SIZE, BUF_SIZE);
        if (ret != 0) {
            fprintf(stderr, "posix_memalign: %s\n", strerror(ret));
            return -ret;
        }
        file_info->buffers[i].len = (i == blocks - 1 && (file_size % BUF_SIZE) ? (file_size % BUF_SIZE) : BUF_SIZE);
        file_info->buffers[i].offset = i * BUF_SIZE;
    }

    return 0;
}

int prepare_io_uring(struct io_uring *io_uring, struct io_uring_params *params) {
    struct io_uring_sq *sring = &io_uring->sq;
    struct io_uring_cq *cring = &io_uring->cq;
    void *sq_ptr = NULL, *cq_ptr = NULL;
    int fd = io_uring_setup(ENTRIES, params);
    if (fd < 0) {
        perror("io_uring_setup: ");
        return -1;
    }
    io_uring->ring_fd = fd;

    // calculate mmap size
    int sring_sz = params->sq_off.array + params->sq_entries * sizeof(unsigned int);
    int cring_sz = params->cq_off.cqes + params->cq_entries * sizeof(struct io_uring_cqe);

    // check single mmap (cqring + sqring), sqes is always uses dedicated mmap
    if (params->features & IORING_FEAT_SINGLE_MMAP) {
        if (cring_sz > sring_sz) {
            sring_sz = cring_sz;
        }
        cring_sz = sring_sz;
    }

    /**
     * mmap sq ring
     */
    sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, io_uring->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        perror("mmap :");
        return -1;
    }

    /**
     * mmap cq ring if IORING_FEAT_SINGLE_MAP is unsupported.
     */
    if (params->features & IORING_FEAT_SINGLE_MMAP) {
        cq_ptr = sq_ptr;
    } else {
        cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, io_uring->ring_fd, IORING_OFF_CQ_RING);
        if (cq_ptr == MAP_FAILED) {
            perror("mmap: ");
            return -1;
        }
    }

    /**
     * configure sqring mmap pointers
     */
    sring->khead = sq_ptr + params->sq_off.head;                             // head number pointer
    sring->ktail = sq_ptr + params->sq_off.tail;                             // tail number pointer
    sring->ring_mask = *(unsigned *)(sq_ptr + params->sq_off.ring_mask);     // ring mask pointer
    sring->ring_entries = *(unsigned *)sq_ptr + params->sq_off.ring_entries; // ring entries pointer
    sring->kflags = sq_ptr + params->sq_off.flags;                           // flags pointer
    sring->array = sq_ptr + params->sq_off.array;                            // array pointer

    /**
     * mmap sqes
     */
    io_uring->sq.sqes = mmap(0, params->sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, io_uring->ring_fd, IORING_OFF_SQES);
    if (io_uring->sq.sqes == MAP_FAILED) {
        perror("mmap: ");
        return -1;
    }

    /**
     * configure cqring mmap pointers
     */
    cring->khead = cq_ptr + params->cq_off.head;                             // head number pointer
    cring->ktail = cq_ptr + params->cq_off.tail;                             // tail number pointer
    cring->ring_mask = *(unsigned *)cq_ptr + params->cq_off.ring_mask;       // ring mask pointer
    cring->ring_entries = *(unsigned *)cq_ptr + params->cq_off.ring_entries; // ring entries pointer
    cring->cqes = cq_ptr + params->cq_off.cqes;                              // cqes pointer

    // printf("cq %d %d %d sq %d %d %d\n", *cring->khead, *cring->ktail, *cring->ring_entries, *sring->khead, *sring->ktail, *sring->ring_entries);

    return 0;
}

int register_file(struct io_uring *io_uring, int fd) {
    return io_uring_register(io_uring->ring_fd, IORING_REGISTER_FILES, &fd, 1);
}

/**
 * check unconsumed sqe
 */
int sq_ready(struct io_uring *io_uring) {
    struct io_uring_sq *sq = &io_uring->sq;
    barrier();
    unsigned sq_head_val = *sq->khead;
    unsigned sq_tail_val = *sq->ktail;
    return (sq_tail_val - sq_head_val);
}

/**
 * check submission queue available entries
 */
int sq_space_left(struct io_uring *io_uring) {
    struct io_uring_sq *sq = &io_uring->sq;
    barrier();
    unsigned sq_head_val = *sq->khead;
    unsigned sq_tail_val = *sq->ktail;
    unsigned sq_entries_val = sq->ring_entries;
    return sq_entries_val - sq_ready(io_uring);
}

/**
 * check submission queue is full
 */
int sq_full(struct io_uring *io_uring) {
    return sq_space_left(io_uring) == 0;
}

/**
 * get sqe
 */
struct io_uring_sqe *sq_get(struct io_uring *io_uring) {
    struct io_uring_sq *sq = &io_uring->sq;
    barrier();
    unsigned sq_head_val = *sq->khead;
    unsigned sq_tail_val = *sq->ktail;
    unsigned sq_next_tail_val = 1 + *sq->ktail;
    unsigned sq_entries_val = sq->ring_entries;
    unsigned sq_mask_val = sq->ring_mask;

    if (sq_space_left(io_uring) < 1) {
        return NULL;
    }
    struct io_uring_sqe *sqe = &io_uring->sq.sqes[(sq_tail_val & sq_mask_val)];
    return sqe;
}

/**
 * check completion queue ready
 */
int cq_ready(struct io_uring *io_uring) {
    struct io_uring_cq *cq = &io_uring->cq;
    barrier();
    unsigned cq_head_val = *cq->khead;
    unsigned cq_tail_val = *cq->ktail;
    // printf("cq_ready %d %d\n", cq_tail_val, cq_head_val);
    return (cq_tail_val - cq_head_val);
}

/**
 * check completion queue available entries
 */
int cq_space_left(struct io_uring *io_uring) {
    struct io_uring_cq *cq = &io_uring->cq;
    barrier();
    unsigned cq_entries_val = cq->ring_entries;
    return cq_entries_val - cq_ready(io_uring);
}

/**
 * check completion queue is full
 */
int cq_full(struct io_uring *io_uring) {
    return cq_space_left(io_uring) == 0;
}

/**
 * submit sqe
 */
int sq_submit(struct io_uring *io_uring) {
    struct io_uring_sq *sq = &io_uring->sq;
    barrier();
    unsigned sq_head_val = *sq->khead;
    unsigned sq_tail_val = *sq->ktail;
    unsigned sq_next_tail_val = 1 + sq_tail_val;
    unsigned sq_entries_val = sq->ring_entries;
    unsigned sq_mask_val = sq->ring_mask;

    if (sq_space_left(io_uring) < 1) {
        return -1;
    }
    // struct io_uring_sqe *sqe = &io_uring->sqes[(sq_tail_val & sq_mask_val)];
    barrier();
    *sq->ktail = sq_next_tail_val; // advanced

    if ((*sq->kflags & IORING_SQ_NEED_WAKEUP) != 0) {
        printf("io_uring_enter\n");
        io_uring_enter(io_uring->ring_fd, sq_ready(io_uring), 0, 0, NULL);
    }

    return 0;
}

/**
 * peek cqe
 */
struct io_uring_cqe *cq_peek(struct io_uring *io_uring) {
    struct io_uring_cq *cq = &io_uring->cq;
    barrier();
    unsigned cq_head_val = *cq->khead;
    unsigned cq_tail_val = *cq->ktail;
    unsigned cq_mask_val = cq->ring_mask;

    struct io_uring_cqe *cqe;

    if (cq_ready(io_uring)) {
        cqe = &cq->cqes[(cq_head_val & cq_mask_val)];
        return cqe;
    }
    return NULL;
}

/**
 * wait cqe, do busy wait.
 */
struct io_uring_cqe *cq_wait(struct io_uring *io_uring) {
    // while (!cq_ready(io_uring)) {
    //     usleep(10);
    // }
    while (1) {
        struct io_uring_cqe *cqe = cq_peek(io_uring);
        if (cqe) {
            return cqe;
        }
    }
    return NULL;
}

/**
 * seen cqe
 */
void cq_seen(struct io_uring *io_uring) {
    struct io_uring_cq *cq = &io_uring->cq;
    barrier();
    unsigned cq_head_val = *cq->khead;
    unsigned cq_next_head_val = cq_head_val + 1;
    barrier();
    *cq->khead = cq_next_head_val;
}

int submit_read(struct io_uring *io_uring, struct buf_info *buf_info, int fd) {
    int uring_fd = io_uring->ring_fd;

    struct io_uring_sqe *sqe;

    while (sq_space_left(io_uring) < 1) {
        struct io_uring_cqe *cqe = cq_wait(io_uring);
        if (cqe == NULL) {
            continue;
        }
        if (cqe->res < 0) {
            fprintf(stderr, "io_uring_cqe: %s\n", strerror(-cqe->res));
        }
        printf("cq_wait %d %x\n", cqe->user_data, cqe);
        cq_seen(io_uring);
    }
    // printf("sq %d %d %d cq %d %d %d\n", *io_uring->sq.head, *io_uring->sq.tail, *io_uring->sq.ring_entries, *io_uring->cq.head, *io_uring->cq.tail, *io_uring->cq.ring_entries);
    sqe = sq_get(io_uring);
    if (sqe == NULL) {
        fprintf(stderr, "sqe get failed\n");
        exit(-1);
    }
    sqe->opcode = IORING_OP_READ;
    sqe->ioprio = 0;
    sqe->fd = fd;
    sqe->off = buf_info->offset;
    sqe->addr = (unsigned long)buf_info->buf;
    sqe->len = buf_info->len;
    sqe->rw_flags = 0;
    sqe->buf_index = 0;
    sqe->personality = 0;
    sqe->file_index = 0;
    sqe->addr3 = 0;
    sqe->__pad2[0] = 0;
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->user_data = (unsigned long)buf_info->offset;

    // printf("submit fd %d offset %d len %d\n", fd, sqe->off, sqe->len);

    sq_submit(io_uring);
}

int read_file(struct io_uring *io_uring, struct file_info *file_info) {
    for (int i = 0; i < file_info->blocks; i++) {
        int buf_index = i;                                        // (i % 2 ? (i / 2) : (file_info->blocks - 1 - (i / 2))); // zigzag pattern
        submit_read(io_uring, &file_info->buffers[buf_index], 0); // registered file descriptor
    }
    while (sq_ready(io_uring)) {
        struct io_uring_cqe *cqe = cq_wait(io_uring);
        if (cqe == NULL) {
            continue;
        }
        if (cqe->res < 0) {
            fprintf(stderr, "cqe: %s\n", strerror(-cqe->res));
        }
        printf("sq_ready %d\n", cqe->user_data);
        cq_seen(io_uring);
    }
    while (cq_ready(io_uring)) {
        struct io_uring_cqe *cqe = cq_wait(io_uring);
        if (cqe == NULL) {
            continue;
        }
        if (cqe->res < 0) {
            fprintf(stderr, "cqe: %s\n", strerror(-cqe->res));
        }
        printf("cq_ready %d\n", cqe->user_data);
        cq_seen(io_uring);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s filename\n", argv[0]);
        return -EINVAL;
    }
    struct file_info *file_info = NULL;

    if (prepare_file(argv[1], &file_info)) {
        fprintf(stderr, "prepare file failed\n");
        return -EINVAL;
    }

    struct io_uring io_uring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(struct io_uring_params));
    params.flags = IORING_SETUP_SQPOLL;  // enable sqpoll
    params.flags |= IORING_SETUP_SQ_AFF; // sqpoll cpu affinity
    params.sq_thread_cpu = 1;            // set core 1
    params.sq_thread_idle = 2000000;     // idle after 2000ms of inactive

    // cpu affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset); // main process cpu affinity

    // setup io_uring
    if (prepare_io_uring(&io_uring, &params)) {
        fprintf(stderr, "io_uring_init failed");
        return -EINVAL;
    }

    // register file
    int ret = register_file(&io_uring, file_info->fd);
    if (ret != 0) {
        fprintf(stderr, "io_uring_register_files: %s\n", strerror(-ret));
        return ret;
    }

    // read zigzag pattern
    read_file(&io_uring, file_info);

    // print buffer
    // for (int i = 0; i < file_info->blocks; i++) {
    //     char *buf = file_info->buffers[i].buf;
    //     size_t len = file_info->buffers[i].len;
    //     for (int j = 0; j < len; j++) {
    //         printf("%x", buf[j]);
    //     }
    // }
    printf("\n");

    return 0;
}