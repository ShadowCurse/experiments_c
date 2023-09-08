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

int main() {
  // CREATE SOCKET
  printf("Creating socket to uffd\n");
  int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    printf("sockfd failed\n");
    return 1;
  }

  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, UFFD_SOCKET_PATH, strlen(UFFD_SOCKET_PATH));

  unlink(UFFD_SOCKET_PATH);

  if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    printf("bind failed");
    return 1;
  }
  printf("socket bind done\n");

  // RECIEVE UFFD FROM FRONT
  printf("Waiting for uffd message from frondend\n");
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
  int uffd = *(int*)CMSG_DATA(cmsg);
  printf("uffd: %d\n", uffd);

  // CREATE AN EMPTY PAGE
  char* page = (char*)mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    perror("uffd thread mmap failed");
    exit(EXIT_FAILURE);
  }

  // Loop, handling incoming events on the userfaultfd file descriptor.
  // Number of faults so far handled
  int fault_cnt = 0; 
  for (;;) {
      // We only trigger NUM_PAGES page faults
      if (fault_cnt == NUM_PAGES) {
        break;
      }

      struct pollfd pollfd;
      pollfd.fd = uffd;
      pollfd.events = POLLIN;
      int nready = poll(&pollfd, 1, -1);
      if (nready == -1) {
        perror("poll failed");
        exit(EXIT_FAILURE);
      }

      printf("\nfault_handler_thread():\n");
      printf("    poll() returns: nready = %d; "
             "POLLIN = %d; POLLERR = %d\n", nready,
             (pollfd.revents & POLLIN) != 0,
             (pollfd.revents & POLLERR) != 0);

      // Read an event from the userfaultfd.
      struct uffd_msg msg;
      int nread = read(uffd, &msg, sizeof(msg));
      if (nread == 0) {
          perror("EOF on userfaultfd");
          exit(EXIT_FAILURE);
      }

      if (nread == -1){
        perror("uffd thread read failed");
        exit(EXIT_FAILURE);
      }

      // We expect only one kind of event; verify that assumption.
      if (msg.event != UFFD_EVENT_PAGEFAULT) {
          perror("Unexpected event on userfaultfd");
          exit(EXIT_FAILURE);
      }

      // Display info about the page-fault event.
      printf("    UFFD_EVENT_PAGEFAULT event: ");
      printf("flags = %"PRIx64"; ", msg.arg.pagefault.flags);
      printf("address = %"PRIx64"\n", msg.arg.pagefault.address);

      //Copy the page pointed to by 'page' into the faulting
      //region. Vary the contents that are copied in, so that it
      //is more obvious that each fault is handled separately. 
      memset(page, 'A' + fault_cnt % 20, PAGE_SIZE);
      fault_cnt++;

      struct uffdio_copy uffdio_copy;
      uffdio_copy.src = (unsigned long) page;

      //We need to handle page faults in units of pages(!).
      //So, round faulting address down to page boundary.

      uffdio_copy.dst = (unsigned long) msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
      uffdio_copy.len = PAGE_SIZE;
      uffdio_copy.mode = 0;
      uffdio_copy.copy = 0;
      if (ioctl(uffd, UFFDIO_COPY, &uffdio_copy) == -1) {
          perror("UFFDIO_COPY");
          exit(EXIT_FAILURE);
      }

      printf("        (uffdio_copy.copy returned %"PRId64")\n",
             uffdio_copy.copy);
  }

  return 0;
}
