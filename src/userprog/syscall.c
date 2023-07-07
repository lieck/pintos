#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "syscall.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  // printf("System call number: %d\n", args[0]);

  if (args[0] == SYS_EXIT) {
    f->eax = args[1];
    printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
    process_exit();
  }

  // TODO(p1-process control syscalls)
  switch (args[0]) {
    case SYS_PRACTICE:
      f->eax = sys_practice(args[1]);
      break;
    case SYS_HALT:
      sys_halt();
      break;
    case SYS_EXIT:
      // sys_exit(args[1]);
      break;
    case SYS_EXEC:
      f->eax = sys_exec(args[1]);
      break;
    case SYS_WAIT:
      f->eax = sys_wait(args[1]);
      break;
    default:
      break;
  }

  // TODO(p1-file operation syscalls)
  switch (args[0]) {
    case SYS_CREATE:
      f->eax = sys_create(args[1], args[2]);
      break;
    case SYS_REMOVE:
      f->eax = sys_remove(args[1]);
      break;
    case SYS_OPEN:
      f->eax = sys_open(args[1]);
      break;
    case SYS_READ:
      f->eax = sys_read(args[1], args[2], args[3]);
      break;
    case SYS_FILESIZE:
      f->eax = sys_filesize(args[1]);
      break;
    case SYS_WRITE:
      f->eax = sys_write(args[1], args[2], args[3]);
      break;
    case SYS_SEEK:
      sys_seek(args[1], args[2]);
      break;
    case SYS_TELL:
      f->eax = sys_tell(args[1]);
      break;
    case SYS_CLOSE:
      sys_close(args[1]);
      break;
    default:
      break;
  }
}

int sys_practice(int i) { return i + 1; }

void sys_halt(void) {}

void sys_exit(int status) {}

pid_t sys_exec(const char* cmd_line) {}

int sys_wait(pid_t pid) {}

int sys_create(const char* file, unsigned initial_size) { return filesys_open(file, initial_size); }

int sys_remove(const char* file) { return filesys_remove(file); }

int sys_open(const char* file) {}

int sys_filesize(int fd) {}

int sys_read(int fd, void* buffer, unsigned size) {}

int sys_write(int fd, const void* buffer, unsigned size) {
  if (fd == 1) {
    printf("%s", buffer);
    return 0;
  }
}

void sys_seek(int fd, unsigned position) {}

unsigned sys_tell(int fd) {}

void sys_close(int fd) {}