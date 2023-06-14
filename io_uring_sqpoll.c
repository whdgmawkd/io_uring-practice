/**
 * random read program using liburing and io_uring with sqpoll enabled.
 */

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