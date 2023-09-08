#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <linux/userfaultfd.h>

const int PAGE_SIZE = 4096;
const int NUM_PAGES = 20;
const int SIZE = PAGE_SIZE * NUM_PAGES;
const char* SERVER_SOCKET_PATH = "test_socket";
const char* UFFD_SOCKET_PATH = "test_socket_uffd";

#define LOG_TIME(fn) \
    struct timeval tv; \
    gettimeofday(&tv,NULL); \
    unsigned long before = 1000000 * tv.tv_sec + tv.tv_usec; \
    fn; \
    gettimeofday(&tv,NULL); \
    unsigned long after = 1000000 * tv.tv_sec + tv.tv_usec; \
    printf("diff: %ld us (%ld ms)\n", after - before, (after - before) / 1000); 

int get_mmfd(int sockfd) {
  printf("Waiting for memfd message\n");
  char iov_dummy;
  struct iovec iov = { 
    .iov_base = &iov_dummy, 
    .iov_len = sizeof(char) 
  };
  char buff[CMSG_SPACE(sizeof(int))];

  struct msghdr msg = {
    .msg_name = 0,
    .msg_namelen = 0,
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = &buff,
    .msg_controllen = sizeof(buff),
    .msg_flags = 0,
  };

  int n = recvmsg(sockfd, &msg, 0);
  printf("Received: %d bytes\n", n);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  int memfd = *(int*)CMSG_DATA(cmsg);
  printf("memfd: %d\n", memfd);

  return memfd;
}

void send_uffd(int sockfd, uint64_t memfd_map, int uffd) {
  struct iovec iov = {
    .iov_base = &memfd_map,
    .iov_len = sizeof(uint64_t),
  };
  char buff[CMSG_SPACE(sizeof(int))];
  struct msghdr msg = {
    .msg_name = 0,
    .msg_namelen = 0,
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = &buff,
    .msg_controllen = sizeof(buff),
    .msg_flags = 0,
  };

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  int* data = (int*)CMSG_DATA(cmsg);
  memcpy(data, &uffd, sizeof(int));

  printf("sending uffd FD: %d\n", uffd);
  printf("sending uffd addres: %p\n", memfd_map);
  int r = sendmsg(sockfd, &msg, 0);
  if (r < 0) {
    perror("sending uffd fd");
    exit(EXIT_FAILURE);
  } 
}

int main() {
  // CREATE SOCKET
  printf("Creating socket\n");
  int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("sockfd failed");
    exit(EXIT_FAILURE);
  }
  printf("socked fd: %d\n", sockfd);

  unlink(SERVER_SOCKET_PATH);

  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, SERVER_SOCKET_PATH, strlen(SERVER_SOCKET_PATH));

  if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }
  printf("socket bind done\n");

  // RECIEVE MEMFD
  int memfd = get_mmfd(sockfd);

  char* memfd_map = (char*)mmap(0, SIZE, PROT_READ, MAP_SHARED, memfd, 0);
  if (memfd_map == MAP_FAILED) {
    perror("memfd map failed");
    exit(EXIT_FAILURE);
  }
  printf("memfd_map: %p\n", memfd_map);

  // CREATE AND REGISTER UFFD
  printf("Creating uffd\n");
  int uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd < 0) {
    perror("uffd creation failed");
    exit(EXIT_FAILURE);
  }
  printf("uffd: %d\n", uffd);

  struct uffdio_api uffdio_api;
  uffdio_api.api = UFFD_API;
  uffdio_api.features = 0;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
    perror("uffd_api failed");
    exit(EXIT_FAILURE);
  }
  printf("uffd_api done\n");

  struct uffdio_register  uffdio_register;
  uffdio_register.range.start = (uint64_t)memfd_map;
  uffdio_register.range.len = SIZE;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    perror("uffd_register failed");
    exit(EXIT_FAILURE);
  }
  printf("uffd_register done\n");

  // CONNECT TO THE SOCKET
  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("connect failed");
    exit(EXIT_FAILURE);
  }

  // SEND UFFD BACK
  printf("Sending uffd back\n");
  send_uffd(sockfd, (uint64_t)memfd_map, uffd);
  
  printf("sleeping for 1 second\n");
  sleep(1);

  // DO PAGE FAULT
  for (int p = 0; p < NUM_PAGES; p++) {
    for (int i = 0; i < 2; i++) {
      char* ptr = memfd_map + PAGE_SIZE * p;
      LOG_TIME(char c = *(ptr))
      printf("Read page: %d, address %p, offset: %d, byte: %c\n", p, ptr, ptr - memfd_map, c);
    }
  }

  munmap(memfd_map, SIZE);
  close(sockfd);
}
