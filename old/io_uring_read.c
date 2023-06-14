#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/io_uring.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define FILE_NAME "1G.bin"
#define FILE_SIZE 1073741824

struct buf_info {
    off_t offset;
    size_t len;
    char *buf;
};

/**
 * pointer to the io_uring's sq ring values
 */
struct my_sq_ring {
    unsigned *head;         // current head value
    unsigned *tail;         // current tail value
    unsigned *ring_mask;    // ring mask value
    unsigned *ring_entries; // ring entries count value
    unsigned *flags;        // ring flags
    unsigned *array;        // ring array
};

/**
 * point to the io_uring's cq ring values
 */
struct my_cq_ring {
    unsigned *head;
    unsigned *tail;
    unsigned *ring_mask;
    unsigned *ring_entries;
    struct io_uring_cqe *cqes;
};

struct io_ring {
    int ring_fd;
    struct my_sq_ring sq_ring;
    struct io_uring_sqe *sqes;
    struct my_cq_ring cq_ring;
};

/**
 * x86 specific read/write barrier
 */
#define read_barrier() __asm__ __volatile__("" :: \
                                                : "memory")
#define write_barrier() __asm__ __volatile__("" :: \
                                                 : "memory")

/**
 * 'io_uring_setup', 'io_uring_register' and 'io_uring_enter' is not a part of glibc.
 * add syscall wrapper
 */

int io_uring_setup(unsigned int entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(unsigned int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

int io_uring_register(unsigned int ring_fd, unsigned int opcode, const void *arg, unsigned int nr_args) {
    return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}

int zigzag_offset(int n, int total) {
    int offset = n / 2 * BUF_SIZE;
    if (n & 1) {
        offset = total - offset - BUF_SIZE;
    }
    return offset;
}

/**
 * sqring available sqe count
 */
int sq_available(struct io_ring *s) {
    struct my_sq_ring *sqring = &s->sq_ring;
    read_barrier();
    unsigned int entries = *sqring->ring_entries;
    unsigned int head = *sqring->head;
    unsigned int tail = *sqring->tail;
    return entries - (tail - head);
}

/**
 * cqring available cqe count
 */
int cq_full(struct io_ring *s) {
    struct my_cq_ring *cqring = &s->cq_ring;
    unsigned int entries = *cqring->ring_entries;
    unsigned int head = *cqring->head;
    unsigned int tail = *cqring->tail;
    return entries - (tail - head);
}

int sq_submit(struct io_ring *s) {
    if (sq_full(s)) {
        return -EAGAIN; // sq full
    }
}

/**
 * initialize io_uring with sqpoll enabeld
 */
int setup_uring(struct io_ring *s) {
    struct my_sq_ring *sring = &s->sq_ring;
    struct my_cq_ring *cring = &s->cq_ring;
    struct io_uring_params p;
    void *sq_ptr, *cq_ptr;

    /**
     * io_uring setup
     */
    memset(&p, 0, sizeof(p));
    p.flags |= IORING_SETUP_SQPOLL; // enable sqpoll
    p.flags |= IORING_SETUP_SQ_AFF; // set sqpoll's cpu affinity
    p.sq_thread_cpu = 1;            // sqpoll thread uses core 1
    p.sq_thread_idle = 2000;        // sqpoll thread sleep after 200ms of inactive
    s->ring_fd = io_uring_setup(32, &p);
    if (s->ring_fd < 0) {
        perror("io_uring_setup");
        return -1;
    }

    // calculate mmap size
    int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned int);
    int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);

    // check single mmap (cqring + sqring), sqes is always uses dedicated mmap
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (cring_sz > sring_sz) {
            sring_sz = cring_sz;
        }
        cring_sz = sring_sz;
    }

    /**
     * mmap sq ring
     */
    sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        perror("mmap :");
        return -1;
    }

    /**
     * mmap cq ring if IORING_FEAT_SINGLE_MAP is unsupported.
     */
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        cq_ptr = sq_ptr;
    } else {
        cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_CQ_RING);
        if (cq_ptr == MAP_FAILED) {
            perror("mmap: ");
            return -1;
        }
    }

    /**
     * configure sqring mmap pointers
     */
    sring->head = sq_ptr + p.sq_off.head;                 // head number pointer
    sring->tail = sq_ptr + p.sq_off.tail;                 // tail number pointer
    sring->ring_mask = sq_ptr + p.sq_off.ring_mask;       // ring mask pointer
    sring->ring_entries = sq_ptr + p.sq_off.ring_entries; // ring entries pointer
    sring->flags = sq_ptr + p.sq_off.flags;               // flags pointer
    sring->array = sq_ptr + p.sq_off.array;               // array pointer

    /**
     * mmap sqes
     */
    s->sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s->ring_fd, IORING_OFF_SQES);
    if (s->sqes == MAP_FAILED) {
        perror("mmap: ");
        return -1;
    }

    /**
     * configure cqring mmap pointers
     */
    cring->head = cq_ptr + p.cq_off.head;                 // head number pointer
    cring->tail = cq_ptr + p.cq_off.tail;                 // tail number pointer
    cring->ring_mask = cq_ptr + p.cq_off.ring_mask;       // ring mask pointer
    cring->ring_entries = cq_ptr + p.cq_off.ring_entries; // ring entries pointer
    cring->cqes = cq_ptr + p.cq_off.cqes;                 // cqes pointer

    return 0;
}

void read_from_cq(struct io_ring *s) {
    struct buf_info *bi;
    struct my_cq_ring *cring = &s->cq_ring;
    struct io_uring_cqe *cqe;
    unsigned int head, reaped = 0;

    head = *cring->head;

    do {
        read_barrier();
        if (head == *cring->tail) {
            // buffer full
        }
        cqe = &cring->cqes[head & *s->cq_ring.ring_mask];
        bi = (struct buf_info *)cqe->user_data;
        if (cqe->res < 0) {
            fprintf(stderr, "CQE Error: %s\n", strerror(-cqe->res));
        }
        head++;
    } while (1);

    *cring->head = head;
    write_barrier();
}

int submit_to_sq(struct io_ring *s) {
    struct buf_info *buf_infos;

    int file_fd = open(FILE_NAME, O_RDONLY | O_DIRECT);
    if (file_fd < 0) {
        perror("open: ");
        return -1;
    }

    struct my_sq_ring *sring = &s->sq_ring;
    unsigned int index = 0, current_block = 0, tail = 0, next_tail = 0;

    off_t file_sz = FILE_SIZE;
    int blocks = (int)FILE_SIZE / BUF_SIZE;
    if (FILE_SIZE % BUF_SIZE)
        blocks++;
    buf_infos = malloc(sizeof(struct buf_info) * blocks);
    if (!buf_infos) {
        fprintf(stderr, "malloc failed\n");
        return -1;
    }
    memset(buf_infos, 0, sizeof(struct buf_info) * blocks);
    for (int i = 0; i < blocks; i++) {
        int file_offset = zigzag_offset(i, FILE_SIZE);
        if (*s->sq_ring.tail - *s->sq_ring.head > 0) {
        }
    }
}

int main() {
}