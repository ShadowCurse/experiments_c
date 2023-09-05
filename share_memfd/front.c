#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/memfd.h>

static int SIZE = 300;
static char* SERVER_SOCKET_PATH = "test_socket";

int main() {
  int memfd = syscall(SYS_memfd_create, "memfd_test", 0);
  if (memfd < 0) {
    printf("memfd failed\n");
    return 1;
  }
  printf("memfd: %d\n", memfd);

  int r = ftruncate(memfd, SIZE);
  if (r < 0) {
    printf("memfd failed\n");
    return 1;
  }

  char* memfd_map = (char*)mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  if (memfd_map == MAP_FAILED) {
    printf("memfd map failed\n");
    return 1;
  }
  printf("memfd_map: %p\n", memfd_map);

  // create socket
  int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    printf("sockfd failed\n");
    return 1;
  }

  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, SERVER_SOCKET_PATH, strlen(SERVER_SOCKET_PATH));

  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    printf("connect failed");
    return 1;
  }

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
  memcpy(data, &memfd, sizeof(int));

  printf("sending FD: %d\n", memfd);
  sendmsg(sockfd, &msg, 0);

  printf("sleeping\n");
  for (int i = 0; i < 10; i++) {
    sprintf(memfd_map, "Lol %d", i);
    sleep(1);
  }

  munmap(memfd_map, SIZE);
  close(memfd);
  close(sockfd);
}
