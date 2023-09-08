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

static int PAGE_SIZE = 4096;
static int SIZE = 8192;
static char* SERVER_SOCKET_PATH = "test_socket";

void* fault_handler_thread(void *arg) {
  printf("Running uffd thread\n");
  int uffd = (int)arg;

  // Create a page that will be copied into the faulting region.
  char* page = (char*)mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    printf("uffd thread mmap failed\n");
    exit(EXIT_FAILURE);
  }

  int fault_cnt = 0;
  for (;;) {
      struct pollfd pollfd;
      pollfd.fd = uffd;
      pollfd.events = POLLIN;
      int nready = poll(&pollfd, 1, -1);
      if (nready == -1) {
        printf("poll failed\n");
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
          printf("uffd thread read empty: EOF\n");
          exit(EXIT_FAILURE);
      }

      if (nread == -1){
        printf("uffd thread read failed\n");
        exit(EXIT_FAILURE);
      }

      // We expect only one kind of event; verify that assumption.
      if (msg.event != UFFD_EVENT_PAGEFAULT) {
          printf("Unexpected event on userfaultfd\n");
          exit(EXIT_FAILURE);
      }

      // Display info about the page-fault event.
      printf("    UFFD_EVENT_PAGEFAULT event: ");
      printf("flags = %"PRIx64"; ", msg.arg.pagefault.flags);
      printf("address = %"PRIx64"\n", msg.arg.pagefault.address);

      // Copy the page pointed to by 'page' into the faulting
      // region. Vary the contents that are copied in, so that it
      // is more obvious that each fault is handled separately.
      memset(page, 'A' + fault_cnt % 20, PAGE_SIZE);
      fault_cnt++;

      struct uffdio_copy uffdio_copy;
      uffdio_copy.src = (unsigned long) page;

      // We need to handle page faults in units of pages(!).
      // So, round faulting address down to page boundary
      uffdio_copy.dst = (unsigned long) msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
      uffdio_copy.len = PAGE_SIZE;
      uffdio_copy.mode = 0;
      uffdio_copy.copy = 0;
      if (ioctl(uffd, UFFDIO_COPY, &uffdio_copy) == -1) {
          printf("UFFDIO_COPY failed\n");
          exit(EXIT_FAILURE);
      }

      printf("        (uffdio_copy.copy returned %"PRId64")\n",
             uffdio_copy.copy);
  }
}

int main() {
  // create memfd
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

  // create uffd
  int uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd < 0) {
    printf("uffd creation failed\n");
    return 1;
  }
  printf("uffd: %d\n", uffd);

  struct uffdio_api uffdio_api;
  uffdio_api.api = UFFD_API;
  uffdio_api.features = 0;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
    printf("uffd_api failed\n");
    return 1;
  }
  printf("uffd_api\n");

  struct uffdio_register  uffdio_register;
  uffdio_register.range.start = (unsigned long) memfd_map;
  uffdio_register.range.len = SIZE;
  uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
  if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
    printf("uffd_register failed\n");
    return 1;
  }
  printf("uffd_register\n");

  // create uffd thread
  pthread_t thr; // ID of thread that handles page faults
  int s = pthread_create(&thr, NULL, fault_handler_thread, (void *) uffd);
  if (s < 0) {
    printf("uffd thread creation failed\n");
    return 1;
  }
  printf("Created uffd thread\n");

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

  // send memfd to the backend
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

  // do page fault
  printf("Reading address %p\n", memfd_map);
  char c = *memfd_map;
  printf("Byte: %c\n", c);

  printf("Reading 10 bytes from memfd_map\n");
  printf("10 bytes of memfd_map: %.*s\n", 10, memfd_map);

  printf("Reading 10 bytes from memfd\n");
  char read_buff[10];
  int n = read(memfd, &read_buff, 10);
  printf("10 bytes of memfd: %.*s\n", 10, &read_buff);

  munmap(memfd_map, SIZE);
  close(memfd);
  close(sockfd);
}
