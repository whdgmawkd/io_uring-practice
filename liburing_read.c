/**
 * random read program using liburing with sqpoll enabled.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <liburing.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define FILE_NAME "1G.bin"
#define FILE_SIZE 1073741824

struct buf_info {
    off_t offset;
    size_t len;
    char *buf;
};

int zigzag_offset(int n, int total) {
    int offset = n / 2 * BUF_SIZE;
    if (n & 1) {
        offset = total - offset - BUF_SIZE;
    }
    return offset;
}

void check_cqe(struct io_uring *ring) {
    struct io_uring_cqe *cqe;
    while (io_uring_sq_ready(ring)) {
        int ret = io_uring_wait_cqe(ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
            // return ret;
        }
        if (cqe->res < 0) {
            fprintf(stderr, "Error in async operation: %s at offset %d\n", strerror(-cqe->res), ((struct buf_info *)cqe->user_data)->offset);
            // return ret;
        }
        // printf("Result of the opertion: %d at offset %d\n", cqe->res, ((struct buf_info *)cqe->user_data)->offset);
        io_uring_cqe_seen(ring, cqe);
    }
}

int sqpoll_read(struct io_uring *ring) {
    struct buf_info *buf_infos = malloc(sizeof(struct buf_info) * (FILE_SIZE / BUF_SIZE));
    for (int i = 0; i < FILE_SIZE / BUF_SIZE; i++) {
        if (posix_memalign(&buf_infos[i].buf, BUF_SIZE, BUF_SIZE)) {
            perror("posix_memalign: ");
        }
        memset(buf_infos[i].buf, 0, BUF_SIZE);
        buf_infos[i].len = BUF_SIZE;
        buf_infos[i].offset = zigzag_offset(i, FILE_SIZE);
    }
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    int fd = open(FILE_NAME, O_RDONLY | __O_DIRECT);
    if (fd < 0) {
        perror("open: ");
        return -errno;
    }
    int ret = io_uring_register_files(ring, &fd, 1);
    if (ret) {
        fprintf(stderr, "io_uring_register_files: %s\n", strerror(-ret));
        return ret;
    }
    for (int i = 0; i < FILE_SIZE / BUF_SIZE; i++) {
        if (io_uring_sq_space_left(ring) == 0) {
            check_cqe(ring);
        }
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            fprintf(stderr, "cannot get sqe\n");
            return -1;
        }
        io_uring_prep_read(sqe, 0, buf_infos[i].buf, buf_infos[i].len, buf_infos[i].offset);
        io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
        io_uring_sqe_set_data(sqe, &buf_infos[i]);
        io_uring_submit(ring);
    }
    check_cqe(ring);
    for (int i = 0; i < FILE_SIZE / BUF_SIZE; i++) {
        char *buf = buf_infos[i].buf;
        int read_completed = 0;
        // for (int j = 0; j < BUF_SIZE; j++) {
        //     if (buf[j] != 0) {
        //         read_completed = 1;
        //         break;
        //     }
        // }
        // if (read_completed) {
        //     printf("Offset %d read completed\n", buf_infos[i].offset);
        // }
        free(buf);
    }
    free(buf_infos);
}

int main(void) {
    struct io_uring ring;
    struct io_uring_params params;

    if (geteuid()) {
        fprintf(stderr, "You need root privileges to run this program.\n");
        return -EACCES;
    }

    // cpu affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    sched_setaffinity(getpid(), sizeof(cpuset), &cpuset);

    memset(&params, 0, sizeof(params));
    params.flags |= IORING_SETUP_SQPOLL; // enable SQPOLL
    params.flags |= IORING_SETUP_SQ_AFF; // sq_thread_cpu affinity
    params.sq_thread_idle = 2000;        // sqpoll kthread go to idle after 2000ms
    params.sq_thread_cpu = 1;

    int ret = io_uring_queue_init_params(32, &ring, &params);
    if (ret) {
        fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
        return -ret;
    }
    sqpoll_read(&ring);
    io_uring_queue_exit(&ring);
    return 0;
}