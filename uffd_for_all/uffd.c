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

int get_fd_and_addr(int sockfd, uint64_t* addr) {
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
  printf("Received: %d bytes\n", n);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  int fd = *(int*)CMSG_DATA(cmsg);

  return fd;
}

int main() {
  // CREATE SOCKET
  printf("Creating socket to uffd\n");
  int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("sockfd failed\n");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, UFFD_SOCKET_PATH, strlen(UFFD_SOCKET_PATH));

  unlink(UFFD_SOCKET_PATH);

  if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }
  printf("socket bind done\n");

  // RECIEVE FRONT UFFD FROM FRONT
  printf("Waiting for front uffd message from frondend\n");
  uint64_t front_addr;
  int front_uffd = get_fd_and_addr(sockfd, &front_addr);
  printf("front_uffd: %d\n", front_uffd);
  printf("front_addr: %p\n", front_addr);

  // RECIEVE FRONT UFFD FROM FRONT
  printf("Waiting for back uffd message from frondend\n");
  uint64_t back_addr;
  int back_uffd = get_fd_and_addr(sockfd, &back_addr);
  printf("back_uffd: %d\n", back_uffd);
  printf("back_addr: %p\n", back_addr);

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
      struct pollfd pollfds[] = {
        {
          .fd = front_uffd,
          .events = POLLIN,
        },
        {
          .fd = back_uffd,
          .events = POLLIN,
        },
      };
      int nready = poll(pollfds, 2, -1);
      if (nready == -1) {
        perror("poll failed");
        exit(EXIT_FAILURE);
      }
      printf("Got %d ready fds\n", nready);

      for (int i = 0; i < 2; i++) {
        if (pollfds[i].revents & POLLIN) {
          struct pollfd ready = pollfds[i];
          int ready_fd = ready.fd;
          printf("Polled fd: %d\n", ready_fd);

          // Read an event from the userfaultfd.
          struct uffd_msg msg;
          int nread = read(ready_fd, &msg, sizeof(msg));
          if (nread == 0) {
              perror("EOF on userfaultfd");
              exit(EXIT_FAILURE);
          }

          if (nread == -1){
            perror("uffd read failed");
            exit(EXIT_FAILURE);
          }

          // We expect only one kind of event; verify that assumption.
          if (msg.event != UFFD_EVENT_PAGEFAULT) {
              perror("Unexpected event on userfaultfd");
              exit(EXIT_FAILURE);
          }

          //Copy the page pointed to by 'page' into the faulting
          //region. Vary the contents that are copied in, so that it
          //is more obvious that each fault is handled separately. 
          memset(page, 'A' + fault_cnt % 20, PAGE_SIZE);
          fault_cnt++;

          struct uffdio_copy uffdio_copy;
          uffdio_copy.src = (unsigned long) page;

          //We need to handle page faults in units of pages(!).
          //So, round faulting address down to page boundary.
          uint64_t page_addr = (uint64_t)msg.arg.pagefault.address & ~(PAGE_SIZE - 1);

          printf("serving page %p\n", page_addr);

          uffdio_copy.dst = page_addr;
          uffdio_copy.len = PAGE_SIZE;
          uffdio_copy.mode = 0;
          uffdio_copy.copy = 0;
          if (ioctl(ready_fd, UFFDIO_COPY, &uffdio_copy) == -1) {
              perror("UFFDIO_COPY");
              printf("Continuing");
              struct uffdio_continue uffdio_continue;
              uffdio_continue.range.start = (unsigned long) msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
              uffdio_continue.range.len = PAGE_SIZE;
              uffdio_continue.mode = 0;
              uffdio_continue.mapped = 0;

              if (ioctl(ready_fd, UFFDIO_CONTINUE, &uffdio_continue) == -1) {
                  perror("UFFDIO_CONTINUE");
                  exit(EXIT_FAILURE);
              }
          }
        }
      }
  }

  return 0;
}
