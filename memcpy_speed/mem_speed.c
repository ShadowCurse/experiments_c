#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/memfd.h>

#define LOG_TIME(fn) \
    struct timeval tv; \
    gettimeofday(&tv,NULL); \
    unsigned long before = 1000000 * tv.tv_sec + tv.tv_usec; \
    fn; \
    gettimeofday(&tv,NULL); \
    unsigned long after = 1000000 * tv.tv_sec + tv.tv_usec; \
    printf("diff: %ld us (%ld ms)\n", after - before, (after - before) / 1000); 

struct FileAndSize {
  char* name;
  unsigned long size;
};

int test_memcpy(struct FileAndSize* file) {
  int memfd = syscall(SYS_memfd_create, file->name, 0);
  if (memfd < 0) {
    printf("memfd failed\n");
    return 1;
  }

  int r = ftruncate(memfd, file->size);
  if (r < 0) {
    printf("memfd failed\n");
    return 1;
  }

  char* memfd_map = (char*)mmap(0, file->size, PROT_READ | PROT_WRITE, MAP_PRIVATE, memfd, 0);
  if (memfd_map == MAP_FAILED) {
    printf("memfd map failed\n");
    return 1;
  }

  int file_fd = open(file->name, O_RDWR, 0);
  if (file_fd < 0) {
    printf("open file failed");
    return 1;
  }

  char* file_map = (char*)mmap(0, file->size, PROT_READ | PROT_WRITE, MAP_PRIVATE, file_fd, 0);
  if (file_map == MAP_FAILED) {
    printf("file map failed\n");
    return 1;
  }

  printf("memcpy: %ld MB\n", file->size / 1024 / 1024);
  LOG_TIME(memcpy(memfd_map, file_map, file->size));

  munmap(memfd_map, file->size);
  munmap(file_map, file->size);
  close(memfd);
  close(file_fd);
}

int test_fread(struct FileAndSize* file) {
  int memfd = syscall(SYS_memfd_create, file->name, 0);
  if (memfd < 0) {
    printf("memfd failed\n");
    return 1;
  }

  int r = ftruncate(memfd, file->size);
  if (r < 0) {
    printf("memfd failed\n");
    return 1;
  }

  char* memfd_map = (char*)mmap(0, file->size, PROT_READ | PROT_WRITE, MAP_PRIVATE, memfd, 0);
  if (memfd_map == MAP_FAILED) {
    printf("memfd map failed\n");
    return 1;
  }

  FILE *file_fd = fopen(file->name, "rb");

  printf("fread\n");
  LOG_TIME(fread(memfd_map, file->size, 1, file_fd));

  munmap(memfd_map, file->size);
  close(memfd);
  fclose(file_fd);
}

int main() {
  struct FileAndSize files[] = {
    {"test_mem_file_250m", 250 * 1024 * 1024},
    {"test_mem_file_500m", 500 * 1024 * 1024},
    {"test_mem_file_1g", 1024 * 1024 * 1024},
    // {"test_mem_file_2g", 2ul * 1024ul * 1024ul * 1024ul},
  };
  for (int i = 0; i < 3; i++) {
    printf("testing: %s\n", files[i].name);
    test_memcpy(&files[i]);
    test_fread(&files[i]);
  }
  return 0;
}
