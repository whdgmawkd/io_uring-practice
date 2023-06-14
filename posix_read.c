#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUF_SIZE 4096

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage %s filename\n", argv[1]);
        return -1;
    }

    int fd = open(argv[1], O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror("open: ");
        return -1;
    }

    struct stat stat;
    if (fstat(fd, &stat)) {
        perror("fstat: ");
        return -1;
    }
    size_t file_size = stat.st_size;
    size_t blocks = file_size / BUF_SIZE + (file_size % BUF_SIZE ? 1 : 0);
    char *buf = calloc(1, file_size);
    for (int i = 0; i < blocks; i++) {
        size_t zigzag_block = (i % 2) ? (blocks - (i / 2) - 1) : (i / 2);
        size_t offset = zigzag_block * BUF_SIZE;
        lseek(fd, offset, SEEK_SET);
        read(fd, buf + offset, (zigzag_block == blocks - 1) ? (file_size % BUF_SIZE) : BUF_SIZE);
    }
    close(fd);
}