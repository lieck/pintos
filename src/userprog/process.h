#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/synch.h"
#include "threads/thread.h"
#include <list.h>
#include <stdint.h>

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

// Maximum number of open files
#define MAX_OPEN_FILE_SIZE 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

/* Predefined file handles. */
#define STDIN_FILENO 0
#define STDOUT_FILENO 1

/* 文件描述符项 */
struct file_info {
  struct list_elem elem;

  int fd;
  struct file* file;
};

/* 子进程/线程的状态信息 */
struct child_status {
  struct list_elem elem;

  tid_t tid;
  bool is_thread; /* 是否为线程 */

  int exit_status; /* 退出的状态 */
  bool active;     /* 是否调用过 wait or join */

  struct process* child_pcb; /* 子进程的 pcb, 表示子进程时有效 */

  struct semaphore sema;
};

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct thread* main_thread; /* Pointer to main thread */

  struct file* elf_file; /* 当前程序的 elf */

  /* 文件相关变量 */
  int next_fd;         /* 下一个 fd */
  struct list fd_list; /* fd table */
  /* 进程工作目录 */
  struct dir* working_directory_pt_;

  struct list thread_list; /* 子进程 or 线程的链表 */

  int active_thread_cnt; /* 活跃的线程数量 */
  int next_stack_idx;    /* 下一个线程使用栈索引 */

  struct list pthread_sync_list;

  struct lock lock;

  bool exit_active;           /* 是否调用了 exit 退出进程 */
  struct semaphore exit_sema; /* 等待线程退出的信号量 */

  struct process* parent_pcb;
  int main_thread_pid;
};

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(int status);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

struct file_info* get_fd(struct process* pcb, int fd);

struct child_status* get_child(struct process* pcb, tid_t tid);

struct pthread_synch_info* get_sync(struct process* pcb, int id);

// 文件系统调用的锁
struct lock sys_file_lock;

#endif /* userprog/process.h */
