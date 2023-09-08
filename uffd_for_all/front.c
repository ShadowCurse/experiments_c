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

static int PAGE_SIZE = 4096;
static int SIZE = 8192;
static char* SERVER_SOCKET_PATH = "test_socket";
static char* UFFD_SOCKET_PATH = "test_socket_uffd";

#define LOG_TIME(fn) \
    struct timeval tv; \
    gettimeofday(&tv,NULL); \
    unsigned long before = 1000000 * tv.tv_sec + tv.tv_usec; \
    fn; \
    gettimeofday(&tv,NULL); \
    unsigned long after = 1000000 * tv.tv_sec + tv.tv_usec; \
    printf("diff: %ld us (%ld ms)\n", after - before, (after - before) / 1000); 

int connect_socket(int sockfd, char* path) {
  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, path, strlen(path));

  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("connect failed");
    exit(EXIT_FAILURE);
  }

  return sockfd;
}

void bind_socket(int sockfd, char* path) {
  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, path, strlen(path));

  if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }
} 

void send_fd(int sockfd, int fd) {
  char iov_dummy = 'A';
  struct iovec iov = {
    .iov_base = &iov_dummy,
    .iov_len = sizeof(char),
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
  memcpy(data, &fd, sizeof(int));

  printf("sending FD: %d\n", fd);
  int r = sendmsg(sockfd, &msg, 0);
  if (r < 0) {
    perror("sending uffd fd");
    exit(EXIT_FAILURE);
  } 
}

void send_fd_and_addr(int sockfd, int fd, uint64_t addr) {
  struct iovec iov = {
    .iov_base = &addr,
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
  memcpy(data, &fd, sizeof(int));

  printf("sending FD: %d\n", fd);
  int r = sendmsg(sockfd, &msg, 0);
  if (r < 0) {
    perror("sending uffd fd");
    exit(EXIT_FAILURE);
  } 
}

int get_uffd_from_backend(int sockfd, uint64_t* addr) {
  struct iovec iov = { 
    .iov_base = addr, 
    .iov_len = sizeof(uint64_t) 
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
  printf("received: %d bytes\n", n);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  int back_uffd = *(int*)CMSG_DATA(cmsg);
  printf("back_uffd: %d", back_uffd);

  return back_uffd;
}

int main() {
  // CREATE LOCAL MEMFD
  printf("Creating memfd\n");
  int memfd = syscall(SYS_memfd_create, "memfd_test", 0);
  if (memfd < 0) {
    perror("memfd failed\n");
    exit(EXIT_FAILURE);
  }
  printf("memfd: %d\n", memfd);

  int r = ftruncate(memfd, SIZE);
  if (r < 0) {
    perror("memfd failed\n");
    exit(EXIT_FAILURE);
  }

  char* memfd_map = (char*)mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  if (memfd_map == MAP_FAILED) {
    perror("memfd map failed");
    exit(EXIT_FAILURE);
  }
  printf("memfd_map: %p\n", memfd_map);

  // CREATE SOCKET
  int back_sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (back_sockfd < 0) {
    perror("back_sockfd failed");
    exit(EXIT_FAILURE);
  }
  printf("back_sockfd: %d", back_sockfd);

  // CONNECT SOCKET TO BACKEND
  printf("Connecting backend socket %s\n", SERVER_SOCKET_PATH);
  connect_socket(back_sockfd, SERVER_SOCKET_PATH);

  // SEND MEMFD TO BACKEND
  printf("Sending memfd\n");
  send_fd(back_sockfd, memfd);

  // UNLINK SOCKET
  unlink(SERVER_SOCKET_PATH);

  // BIND SOCKET
  printf("Bingin backend socket: %s\n", SERVER_SOCKET_PATH);
  bind_socket(back_sockfd, SERVER_SOCKET_PATH);

  // RECIEVE UFFD FROM BACKEND
  printf("Waiting for uffd message\n");
  uint64_t back_uffd_addr;
  int back_uffd = get_uffd_from_backend(back_sockfd, &back_uffd_addr);
  printf("back_uffd: %d\n", back_uffd);
  printf("back_uffd_addr: %p\n", back_uffd_addr);

  // CREATE LOCAL UFFD
  printf("Creating and registering uffd\n");
  int local_uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (local_uffd < 0) {
    perror("uffd creation failed");
    exit(EXIT_FAILURE);
  }
  printf("local_uffd: %d\n", local_uffd);

  struct uffdio_api uffdio_api;
  uffdio_api.api = UFFD_API;
  uffdio_api.features = 0;
  if (ioctl(local_uffd, UFFDIO_API, &uffdio_api) == -1) {
    perror("uffd_api failed");
    exit(EXIT_FAILURE);
  }
  printf("uffd_api done\n");

  struct uffdio_register  uffdio_register;
  uffdio_register.range.start = (unsigned long) memfd_map;
  uffdio_register.range.len = SIZE;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
  if (ioctl(local_uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    printf("uffd_register failed\n");
    exit(EXIT_FAILURE);
  }
  printf("uffd_register done\n");

  // CREATE UFFD SOCKET
  int uffd_sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (uffd_sockfd < 0) {
    perror("uffd_sockfd failed\n");
    exit(EXIT_FAILURE);
  }

  // CONNECT UFFD SOCKET TO UFFD
  printf("Connecting uffd socket: %s\n", UFFD_SOCKET_PATH);
  connect_socket(uffd_sockfd, UFFD_SOCKET_PATH);

  // SEND LOCAL UFFD
  printf("Sending local_uffd\n");
  send_fd_and_addr(uffd_sockfd, local_uffd, (uint64_t)memfd_map);
  send_fd_and_addr(uffd_sockfd, back_uffd, back_uffd_addr);

  printf("Sleeping for 2 seconds");
  // by this time back should have triggered page fault for 1st page
  sleep(2);

  // DO PAGE FAULT
  for (int p = 0; p < 2; p++) {
    for (int i = 0; i < 10; i++) {
      char* ptr = memfd_map + PAGE_SIZE * p;
      printf("Reading page: %d, address %p, offset: %d\n", p, ptr, ptr - memfd_map);
      LOG_TIME(char c = *(ptr))
      printf("Byte: %c\n", c);
    }
  }

  munmap(memfd_map, SIZE);
  close(memfd);
  close(back_sockfd);
  close(uffd_sockfd);
}
