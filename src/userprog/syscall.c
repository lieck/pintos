#include "userprog/syscall.h"
#include <list.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "float.h"
#include "stdbool.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "syscall.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "lib/kernel/stdio.h"
#include "devices/input.h"
#include "threads/malloc.h"
#include "user_synch.h"

static void syscall_handler(struct intr_frame*);

/* Accessing user memory | Pintos Documentation
  https://cs162.org/static/proj/pintos-docs/docs/userprog/accessing-user-mem/

  Each of these functions assumes that the user address has already been verified to be below PHYS_BASE. */
static bool put_user(uint8_t* udst, uint8_t byte);
static int get_user(const uint8_t* uaddr);

static void check_str(const char* ptr);
static void check_num32(uint8_t* ptr);
static bool check_char_ptr(char *ptr);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);

  /* 判断 thread 是否退出 */
  struct thread* curr = thread_current();
  lock_acquire(&curr->pcb->lock);
  if(curr->pcb->exit_active) {
    lock_release(&curr->pcb->lock);
    pthread_exit();
  }
  lock_release(&curr->pcb->lock);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  // printf("System call number: %d\n", args[0]);

  check_num32(args);

  switch (args[0]) {
    case SYS_PRACTICE:
      check_num32(args + 1);
      f->eax = sys_practice(args[1]);
      break;
    case SYS_COMPUTE_E:
      f->eax = sys_compute_e(args[1]);
      break;
    case SYS_HALT:
      sys_halt();
      break;
    case SYS_EXIT:
      check_num32(args + 1);
      sys_exit(args[1]);
      break;
    case SYS_EXEC:
      check_num32(args + 1);
      check_str(args[1]);
      f->eax = sys_exec(args[1]);
      break;
    case SYS_WAIT:
      check_num32(args + 1);
      f->eax = sys_wait(args[1]);
      break;
    default:
      break;
  }

  // TODO(p1-file operation syscalls)
  switch (args[0]) {
    case SYS_CREATE:
      check_num32(args + 1);
      check_num32(args + 2);
      f->eax = sys_create(args[1], args[2]);
      break;
    case SYS_REMOVE:
      check_num32(args + 1);
      f->eax = sys_remove(args[1]);
      break;
    case SYS_OPEN:
      check_num32(args + 1);
      f->eax = sys_open(args[1]);
      break;
    case SYS_READ:
      check_num32(args + 1);
      check_num32(args + 2);
      check_num32(args + 3);
      f->eax = sys_read(args[1], args[2], args[3]);
      break;
    case SYS_FILESIZE:
      check_num32(args + 1);
      f->eax = sys_filesize(args[1]);
      break;
    case SYS_WRITE:
      check_num32(args + 1);
      check_num32(args + 2);
      check_num32(args + 3);
      f->eax = sys_write(args[1], args[2], args[3]);
      break;
    case SYS_SEEK:
      check_num32(args + 1);
      check_num32(args + 2);
      sys_seek(args[1], args[2]);
      break;
    case SYS_TELL:
      check_num32(args + 1);
      f->eax = sys_tell(args[1]);
      break;
    case SYS_CLOSE:
      check_num32(args + 1);
      sys_close(args[1]);
      break;
    default:
      break;
  }


  // user thread
  switch (args[0]) {
  case SYS_PT_CREATE:
    check_num32(args + 1);
    check_num32(args + 2);
    check_num32(args + 3);
    f->eax = pthread_execute((stub_fun)args[1], (pthread_fun)args[2], (void*)args[3]);
    break;
  case SYS_PT_EXIT:
    pthread_exit();
    break;
  case SYS_PT_JOIN:
    check_num32(args + 1);
    f->eax = pthread_join(args[1]);
    break;
  case SYS_GET_TID:
      f->eax = thread_current()->tid;
      break;
  }

  switch (args[0]) {
    case SYS_CHDIR:
      check_num32(args + 1);
      check_str(args + 1);
      f->eax = sys_chdir(args + 1);
      break;
    case SYS_MKDIR:
      check_num32(args + 1);
      check_str(args + 1);
      f->eax = sys_mkdir((char*)args[1]);
      break;
    case SYS_ISDIR:
      check_num32(args + 1);
      check_str(args + 1);
      f->eax = sys_isdir(args + 1);
      break;
    case SYS_INUMBER:
      check_num32(args + 1);
      f->eax = sys_inumber(args[1]);
      break;
    case SYS_READDIR:
      check_num32(args + 1);
      check_char_ptr(args + 2);
      f->eax = sys_readdir(args[1], (char*)args[2]);
      break;
  }

  /* User-level synchronization syscalls */
  if(args[0] >= SYS_LOCK_INIT && args[0] <= SYS_SEMA_UP) {
    /* check ptr */
    check_num32(args + 1);
    char *ptr = (void*)args[1];
    if(!check_char_ptr(ptr)) {
      f->eax = false;
      return;
    }
    
    switch (args[0]) {
      case SYS_LOCK_INIT:
      f->eax = pthread_lock_init(ptr);
      break;
    case SYS_LOCK_ACQUIRE:
      f->eax = pthread_lock_acquire(ptr);
      break;
    case SYS_LOCK_RELEASE:
      f->eax = pthread_lock_release(ptr);
      break;
    case SYS_SEMA_INIT:
      check_num32(args + 2);
      f->eax = pthread_sema_init(ptr, args[2]);
      break;
    case SYS_SEMA_DOWN:
      f->eax = pthread_sema_down(ptr);
      break;
    case SYS_SEMA_UP:
      f->eax = pthread_sema_up(ptr);
      break;
    }
  }

  /* 判断 thread 是否退出 */
  lock_acquire(&curr->pcb->lock);
  if(curr->pcb->exit_active) {
    lock_release(&curr->pcb->lock);
    pthread_exit();
  }
  lock_release(&curr->pcb->lock);
}

int sys_practice(int i) { return i + 1; }

void sys_halt(void) { shutdown_power_off(); }

void sys_exit(int status) {
  process_exit(status);
}

pid_t sys_exec(const char* cmd_line) {
  return process_execute(cmd_line);
}

// TODO(p1-process control syscalls) 等待子进程 pid 终止
int sys_wait(pid_t pid) {
  return process_wait(pid);
}

int sys_create(const char* file, unsigned initial_size) {
  check_str(file);

  lock_acquire(&sys_file_lock);
  int ret = filesys_create(file, initial_size);
  lock_release(&sys_file_lock);
  return ret;
}

int sys_remove(const char* file) {
  check_str(file);

  lock_acquire(&sys_file_lock);
  int ret = filesys_remove(file);
  lock_release(&sys_file_lock);
  return ret;
}

// 打开文件并获取 file 存储到描述符表中
// TODO 假如打开已经打开的文件应该如何处理？
int sys_open(const char* file) {
  check_str(file);

  int fd = -1;
  struct process* p = thread_current()->pcb;

  lock_acquire(&sys_file_lock);

  struct file* f = filesys_open(file);
  if (f == NULL) {
    lock_release(&sys_file_lock);
    return fd;
  }

  // 选择下一个描述符
  fd = p->next_fd++;
  struct file_info *f_info = malloc(1 * sizeof(struct file_info));
  f_info->file = f;
  f_info->fd = fd;

  list_push_front(&p->fd_list, &f_info->elem);
  
  lock_release(&sys_file_lock);
  return fd;
}

int sys_filesize(int fd) {
  struct process* p = thread_current()->pcb;

  struct file_info* f_info = get_fd(p, fd);
  if(f_info == NULL) {
    return -1;
  }

  lock_acquire(&sys_file_lock);
  int result = file_length(f_info->file);
  lock_release(&sys_file_lock);
  return result;
}

int sys_read(int fd, void* buffer, unsigned size) {
  check_str(buffer);

  if(fd == STDOUT_FILENO) {
    sys_exit(-1);
  }

  if(fd == STDIN_FILENO) {
    for(size_t i = 0; i < size; i++) {
      char c = input_getc();
      ((char*)buffer)[i] = c;
    }
  }

  struct process* p = thread_current()->pcb;

  struct file_info* f_info = get_fd(p, fd);
  if(f_info == NULL) {
    return -1;
  }

  // TODO 是否有不需要临时缓存区的方法？
  char* tem_buf = malloc(size * sizeof(char));

  lock_acquire(&sys_file_lock);
  int read_size = file_read(f_info->file, tem_buf, size);
  lock_release(&sys_file_lock);

  for (int i = 0; i < read_size; i++) {
    // put_user返回值是!=-1 此处改为!put_user
    if (!put_user(buffer + i, tem_buf[i])) {
      free(tem_buf);
      return -1;
    }
  }

  free(tem_buf);
  return read_size;
}

int sys_write(int fd, const void* buffer, unsigned size) {
  check_str(buffer);



  if (fd == STDIN_FILENO) {
    sys_exit(-1);
  }

  if (fd == 1) {
    size_t buf_size = strlen(buffer);
    putbuf(buffer, buf_size);
    return buf_size;
  }

  struct process* p = thread_current()->pcb;

  struct file_info* f_info = get_fd(p, fd);
  if(f_info == NULL) {
    return -1;
  }

  lock_acquire(&sys_file_lock);
  int write_size = file_write(f_info->file, buffer, size);
  lock_release(&sys_file_lock);
  return write_size;
}

void sys_seek(int fd, unsigned position) {
  struct process* p = thread_current()->pcb;

  struct file_info* f_info = get_fd(p, fd);
  if(f_info == NULL) {
    return;
  }

  lock_acquire(&sys_file_lock);
  file_seek(f_info->file, position);
  lock_release(&sys_file_lock);
}

unsigned sys_tell(int fd) {
  struct process* p = thread_current()->pcb;

  struct file_info* f_info = get_fd(p, fd);
  if(f_info == NULL) {
    return -1;
  }

  lock_acquire(&sys_file_lock);
  unsigned result = file_tell(f_info->file);
  lock_release(&sys_file_lock);
  return result;
}

void sys_close(int fd) {
  struct process* p = thread_current()->pcb;

  struct file_info* f_info = get_fd(p, fd);
  if(f_info == NULL) {
    return;
  }

  lock_acquire(&sys_file_lock);
  file_close(f_info->file);
  lock_release(&sys_file_lock);

  list_remove(&f_info->elem);
  free(f_info);
}

double sys_compute_e(int n) {
  return sys_sum_to_e(n);
}

static void find_dir_by_name(const char* dir_name, struct dir* dir) {
  int dir_fd = sys_open(dir_name);
  if (dir_fd == -1) {
    dir = NULL; }
  
  int dir_inum = sys_inumber(dir_fd);

  struct inode* dir_inode = inode_open(dir_inum);
  if (!dir_inode) {
    dir = NULL; }

  dir = dir_open(dir_inode);
}

bool sys_isdir(const char* dir) {
  struct dir* directory;
  find_dir_by_name(dir, directory);
  return directory != NULL;
}

bool sys_chdir(const char* dir) {
  if (!dir || sys_isdir(dir)) {
    return false; }

  struct dir* directory;
  find_dir_by_name(dir, directory);
  if (!directory) {
    return false; }

  struct process* p = thread_current()->pcb;
  p->working_directory_pt_ = directory;
}

int sys_inumber(int fd) {
  struct process* p = thread_current()->pcb;
  if (!p) {
    return false; }

  struct file_info* fd_info = get_fd(p, fd);
  if (!fd_info) {
    return -1; }

  struct inode* fd_inode = file_get_inode(fd_info->file);
  return inode_get_inumber(fd_inode);
}

bool sys_readdir(int fd, char* name) {
  int dir_inum = sys_inumber(fd);

  struct inode* dir_inode = inode_open(dir_inum);
  if (!dir_inode) {
    return false; }

  struct dir* dir_pt = dir_open(dir_inode);
  if (!dir_pt) {
    return false; }

  return dir_readdir(dir_pt, name);
}

bool sys_mkdir(const char* dir) {
  struct dir* working_dir_pt = thread_current()->pcb->working_directory_pt_;
  /* 解析路径 */

  block_sector_t inode_sector = 0;
  bool success = (free_map_allocate(1, &inode_sector)
    && dir_create(inode_sector, 16));
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);

  success = success && dir_add(working_dir_pt, dir, inode_sector);

  return success;
}

/* 校验 args 是否正确 */
static void check_num32(uint8_t* ptr) {
  // 校验 args[0]
  for (size_t i = 0; i < 4; i++) {
    if (ptr + i >= PHYS_BASE)
      goto exit;
    if (get_user(ptr + i) == -1)
      goto exit;
  }
  return;
exit:
  sys_exit(-1);
}

/* 校验字符串是否正确 */
static void check_str(const char* ptr) {
  for (;;) {
    if (ptr >= PHYS_BASE)
      goto exit;
    int val = get_user(ptr);
    if (val == -1)
      goto exit;
    if (val == '\0')
      return;
    ptr++;
  }

exit:
  sys_exit(-1);
}

/* 判断 ptr 指向的地址是否有效 */
static bool check_char_ptr(char *ptr) {
  if (ptr >= PHYS_BASE || get_user(ptr) == -1) {
    return false;
  }
  return true;
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful,
   -1 if a segfault occurred. */
static int get_user(const uint8_t* uaddr) {
  int result;
  asm("movl $1f, %0; movzbl %1, %0; 1:" : "=&a"(result) : "m"(*uaddr));
  return result;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful,
   false if a segfault occurred. */
static bool put_user(uint8_t* udst, uint8_t byte) {
  int error_code;
  asm("movl $1f, %0; movb %b2, %1; 1:" : "=&a"(error_code), "=m"(*udst) : "q"(byte));
  return error_code != -1;
}