#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/memfd.h>

static int PAGE_SIZE = 4096;
static int SIZE = 8192;
static char* SERVER_SOCKET_PATH = "test_socket";

int main() {
  // create socket
  int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    printf("sockfd failed");
    return 1;
  }
  printf("socked fd: %d\n", sockfd);

  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, SERVER_SOCKET_PATH, strlen(SERVER_SOCKET_PATH));

  unlink(SERVER_SOCKET_PATH);

  if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    printf("bind failed");
    return 1;
  }

  // get memfd from frontend
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

  printf("waiting for message..\n");
  int n = recvmsg(sockfd, &msg, 0);
  printf("Received: %d bytes\n", n);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  int memfd = *(int*)CMSG_DATA(cmsg); // receive file descriptor
  printf("memfd: %d\n", memfd);

  char* memfd_map = (char*)mmap(0, SIZE, PROT_READ, MAP_SHARED, memfd, 0);
  if (memfd_map == MAP_FAILED) {
    printf("memfd map failed\n");
    return 1;
  }
  printf("memfd_map: %p\n", memfd_map);

  // sleep to give time for frontend to make an uffd page fault
  sleep(1);
  printf("Reading first 10 bytes\n");
  printf("Message: %.*s\n", 10, memfd_map);

  printf("Reading first 10 bytes of second page\n");
  printf("Message: %.*s\n", 10, memfd_map + PAGE_SIZE);

  munmap(memfd_map, SIZE);
  close(sockfd);
}
