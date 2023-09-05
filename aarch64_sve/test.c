#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_RESET   "\x1b[0m"

void test_sve() {
  asm("ld1d z1.d, p0/z, [x0, x4, lsl 3]");
  asm("ptrue p0.s");
}

void test_mov() {
  int result;
  asm("movz %w[res], #10" : [res] "=r" (result));
}

int fork_test(void(*test_fn)(void)) {
    int pid = fork();
    if (pid == 0) {
      test_fn();
      exit(0);
    } else {
      int wstatus;
      waitpid(pid, &wstatus, 0);
      return WEXITSTATUS(wstatus);
    }
}

void catch_sigill(int signo) {
  exit(69);
}

typedef struct {
  void(*fn)();
  char* name;
} Test;

static Test tests[] = {
  { .fn = test_mov, .name = "mov" },
  { .fn = test_sve, .name = "sve" },
};

int main(int argc, char** argv) {
  struct sigaction new_action;

  new_action.sa_handler = catch_sigill;
  sigemptyset (&new_action.sa_mask);
  new_action.sa_flags = 0;

  printf("Setting SIGILL action\n");
  if (sigaction(SIGILL, &new_action, NULL) < 0) {
    printf("Error setting new action");
    return EXIT_FAILURE;
  }

  printf("Starting testing...\n");
  for (int i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
    printf("Testing %s ... \n", tests[i].name);
    int ret = fork_test(tests[i].fn);
    if (ret == 69) {
      printf(ANSI_COLOR_RED);
      printf("failed\n");
      printf(ANSI_COLOR_RESET);
    } else {
      printf(ANSI_COLOR_GREEN);
      printf("succeded\n");
      printf(ANSI_COLOR_RESET);
    }
  }

  return 0;
}
