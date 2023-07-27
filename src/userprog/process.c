#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <limits.h>
#include <list.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "syscall.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "user_synch.h"

static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp, int idx, void** args);
char* init_stack_frame(const char* name, char* stack);
void pthread_exit_t(void);

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = malloc(sizeof(struct process));
  success = t->pcb != NULL;

  /* Kill the kernel if we did not succeed */
  ASSERT(success);

  t->pcb->pagedir = NULL;
  t->pcb->main_thread = NULL;
  t->pcb->elf_file = NULL;
  list_init(&t->pcb->thread_list);
  lock_init(&t->pcb->lock);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
/* 创建线程运行 file_name, 返回创建线程的 pid */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;

  // 第一次调用时，初始化 process 相关
  static bool init = false;
  if (!init) {
    init = true;
    lock_init(&sys_file_lock);
  }

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL) {
    return TID_ERROR;
  }
  strlcpy(fn_copy, file_name, PGSIZE);

  // name 可能由多个字符串构成, thread name 只取第一个字符串
  // 例如 name = "stack-align-3 a b" 的 thread name 为 "stack-align-3"
  // 这里限制 name 最大为 100，如果需要更改则还要改后面的调用
  ASSERT(strlen(file_name) < 100);
  char thread_name[100];
  size_t split_idx = 0;
  while(file_name[split_idx] != ' ' && file_name[split_idx] != '\0') {
    split_idx++;
  }
  memcpy(thread_name, file_name, split_idx);
  thread_name[split_idx] = '\0';

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create(thread_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR) {
    palloc_free_page(fn_copy);
    return TID_ERROR;
  }

  struct thread* curr = thread_current();
  if(curr->pcb != NULL) {
    struct child_status *cs = get_child(curr->pcb, tid);
    ASSERT(cs != NULL);

    sema_down(&cs->sema);

    if(cs->exit_status == TID_ERROR) {
      list_remove(&cs->elem);
      free(cs);
      return TID_ERROR;
    }
  }
  
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* file_name_) {
  char* file_name = (char*)file_name_;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    list_init(&new_pcb->fd_list);
    list_init(&new_pcb->thread_list);
    list_init(&new_pcb->pthread_sync_list);

    new_pcb->active_thread_cnt = 1;
    new_pcb->next_stack_idx = 1;
    new_pcb->exit_active = false;

    new_pcb->main_thread_pid = t->tid;
    new_pcb->parent_pcb = t->parent->pcb;

    lock_init(&new_pcb->lock);

    sema_init(&new_pcb->exit_sema, 0);

    // 0 和 1 为特殊 fd
    new_pcb->next_fd = 2;
    new_pcb->elf_file = NULL;

    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;

    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;

    // 保护可执行文件
    lock_acquire(&sys_file_lock);
    success = load(file_name, &if_.eip, &if_.esp);
    lock_release(&sys_file_lock);
  }

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  /* Clean up. Exit on failure or jump to userspace */
  palloc_free_page(file_name);
  if (!success) {
    /* 唤醒父进程，并设置退出的状态 */
    if (t->parent != NULL) {
      struct child_status* cs = get_child(t->parent->pcb, t->tid);
      cs->exit_status = -1;
      sema_up(&cs->sema);
    }

    thread_exit();
  }

  if (t->parent != NULL) {
    struct child_status* cs = get_child(t->parent->pcb, t->tid);
    cs->child_pcb = t->pcb;
    sema_up(&cs->sema);
  }

  /* 将 main 线程添加 thread_list 中 */
  struct child_status* cs = malloc(sizeof(struct child_status));
  ASSERT(cs != NULL);
  cs->tid = t->tid;
  cs->is_thread = true;
  cs->active = false;
  cs->exit_status = 0;
  sema_init(&cs->sema, 0);
  list_push_front(&t->pcb->thread_list, &cs->elem);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid) {
  struct thread* curr = thread_current();

  lock_acquire(&curr->pcb->lock);
  struct child_status* cs = get_child(curr->pcb, child_pid);

  /* 不存在子进程或调用过 wait */
  if(cs == NULL || cs->active) {
    lock_release(&curr->pcb->lock);
    return -1;
  }
  lock_release(&curr->pcb->lock);

  sema_down(&cs->sema);
  
  lock_acquire(&curr->pcb->lock);
  int ret = cs->exit_status;
  list_remove(&cs->elem);
  lock_release(&curr->pcb->lock);

  free(cs);
  return ret;
}

/* Free the current process's resources. */
void process_exit(int status) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  lock_acquire(&cur->pcb->lock);
  if(cur->pcb->exit_active) {
    lock_release(&cur->pcb->lock);
    pthread_exit_t();
    NOT_REACHED();
  }

  ASSERT(!cur->pcb->exit_active);
  cur->pcb->exit_active = true;

  /* 唤醒被用户态锁阻塞的线程 */
  {
    struct list_elem *iter = list_begin(&cur->pcb->pthread_sync_list);
    for(; iter != list_end(&cur->pcb->pthread_sync_list); ) {
      struct pthread_synch_info* psi = list_entry(iter, struct pthread_synch_info, elem);
      while (!list_empty(&psi->waiters)) {
        struct thread *t = list_entry(list_pop_front(&psi->waiters), struct thread, elem);
        thread_unblock(t);
      }
      iter = list_next(iter);
    }
  }

  /* 唤醒调用 join/wait 的线程 */
  {
    struct list_elem *iter = list_begin(&cur->pcb->thread_list);
    for(; iter!= list_end(&cur->pcb->thread_list); ) {
      struct child_status* cs = list_entry(iter, struct child_status, elem);
      if(cs->active) {
        sema_up(&cs->sema);
      }
      iter = list_next(iter);
    }
  }

  /* 等待所有线程退出 */
  while(cur->pcb->active_thread_cnt > 1) {
    lock_release(&cur->pcb->lock);
    sema_down(&cur->pcb->exit_sema);
    lock_acquire(&cur->pcb->lock);
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;

  printf("%s: exit(%d)\n", cur->pcb->process_name, status);

  /* 通知子进程, 父进程已经退出, 并释放 thread_list */
  {
    struct list_elem *iter = list_begin(&cur->pcb->thread_list);
    while (iter!= list_end(&cur->pcb->thread_list)) {
      struct child_status* cs = list_entry(iter, struct child_status, elem);
      if(!cs->is_thread) {
        cs->child_pcb = NULL;
      }
      iter = list_remove(iter);
      free(cs);
    }
  }

  /* 通知父进程自己的退出状态 */
  if(cur->pcb->parent_pcb != NULL) {
    struct child_status *cs = get_child(cur->pcb->parent_pcb, cur->pcb->main_thread_pid);
    ASSERT(cs != NULL);
    cs->exit_status = status;
    cs->child_pcb = NULL;
    sema_up(&cs->sema);
  }

  lock_acquire(&sys_file_lock);

  /* 取消保护 ELF 文件 */
  file_close(cur->pcb->elf_file);

  /* 清理打开的文件集合, 并释放 fd_list */
  {
    struct list_elem *iter = list_begin(&cur->pcb->fd_list);
    for(; iter != list_end(&cur->pcb->fd_list); ) {
      struct file_info* f_info = list_entry(iter, struct file_info, elem);
      iter = list_remove(iter);
      file_close(f_info->file);
      free(f_info);
    }
    lock_release(&sys_file_lock);
  }

  /* 释放 sync_list */
  {
    struct list_elem *iter = list_begin(&cur->pcb->pthread_sync_list);
    for(; iter != list_end(&cur->pcb->pthread_sync_list); ) {
      struct pthread_synch_info* f_info = list_entry(iter, struct pthread_synch_info, elem);
      iter = list_remove(iter);
      free(f_info);
    }
  }

  cur->pcb = NULL;
  free(pcb_to_free);
  
  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

// 去除参数中多余的空格
// 通过args-*的测试
char* remove_extra_spaces(const char* name) {
  char* new_name = (char*)malloc(strlen(name) + 1);
  int i, j;
  for (i = 0, j = 0; name[i] != '\0'; i++) {
    if (name[i] != ' ') {
      new_name[j++] = name[i];
    } else if (j > 0 && new_name[j - 1] != ' ') {
      new_name[j++] = ' ';
    }
  }
  if (j > 0 && new_name[j - 1] == ' ') {
    j--;
  }
  new_name[j] = '\0';
  return new_name;
}

// 初始化 stack frame
char* init_stack_frame(const char* name, char* stack) {
  // 解析 file_name, 例如 ls -ahl
  size_t name_len = strlen(name) + 1;

  // 获取去除了空格之后的name
  char* new_name = remove_extra_spaces(name);

  name_len = strlen(new_name) + 1;

  // 空间对齐后参数们所需要的空间
  size_t argument_string_memory = (name_len / 4 + (name_len % 4 ? 1 : 0)) * 4;
  // 拷贝到栈上
  memcpy(stack - name_len, new_name, name_len);

  free(new_name);
  
  memset(stack - argument_string_memory, 0, argument_string_memory - name_len);

  size_t argument_count = 1;
  char* argument_idx_offset[30] = {[0] =
                                       stack - name_len}; // 最多三十个参数 TODO: 参数个数的预估方法
  // 将 ' ' 替换为 '\0' (ls -alh -> ls\0lalh), 同时统计参数个数及其偏移
  for (size_t idx = 0; idx < name_len - 1 /* 防止最后一个'\0'被统计 */; ++idx) {
    char* currunt_position = stack + idx - name_len;
    if (*currunt_position == ' ') {
      *currunt_position = '\0';
      if (idx + 1 < name_len - 1 &&
          *(currunt_position + 1) != ' ') // TODO: 应该在入参处去除重复的多余的空格
        argument_idx_offset[argument_count++] = currunt_position + 1;
    }
  }
  argument_idx_offset[argument_count++] = 0; // 保护位, 可以舍弃
  // 设置指向各个参数字符数组的指针
  size_t point_argument_memory_len = sizeof(char*) * argument_count;
  char* point_argument_memory_start = stack - argument_string_memory - point_argument_memory_len;
  memcpy(point_argument_memory_start, argument_idx_offset, point_argument_memory_len);

  void** return_argc_argv =
      point_argument_memory_start -
      sizeof(void*) * 3; // sizeof(int) == sizeof(void*) 所以  argc 也在这里一起处理了
  size_t mem_to_align = (size_t)return_argc_argv % 16 + 4;
  if (mem_to_align == 16)
    mem_to_align = 0;
  return_argc_argv -= mem_to_align / sizeof(void*); // Align to 0xc
  if (mem_to_align > 0) {
    memset(return_argc_argv + 3, 0, mem_to_align);
  }

  return_argc_argv[2] = point_argument_memory_start; // argv
  return_argc_argv[1] = (void*)argument_count - 1;   // argc
  return_argc_argv[0] = 0;                           // return address

  return return_argc_argv;
}

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  // TODO(p1-argument passing) 对于 stack-align-2 a 的参数需要解析为 stack-align-2
  size_t split_idx = 0;
  while (file_name[split_idx] != ' ' && file_name[split_idx] != '\0')
    split_idx++;
  char command[100];
  memcpy(&command, file_name, split_idx);
  command[split_idx] = '\0';

  file = filesys_open(command);
  if (file == NULL) {
    printf("load: %s: open failed\n", command);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* TODO(pro1-argument passing) load command */
  *esp = init_stack_frame(file_name, *esp);

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;


  t->pcb->elf_file = file;
  file_deny_write(file);
  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  if(!success) {
    file_close(file);
  }
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void), void** esp, int idx, void** args) {
  ASSERT(idx > 1);
  /* 从 pool 获取可用的空闲页面 */
  uint8_t *kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if(kpage == NULL) {
    return false;
  }

  if(!install_page(((uint8_t*)PHYS_BASE) - idx * PGSIZE, kpage, true)) {
    palloc_free_page(kpage);
    return false;
  }

  *esp = PHYS_BASE - (idx - 1) * PGSIZE;
  *esp -= 6 * sizeof(void*);

  /* 初始化栈帧布局为
                       +----------------+
                       |        arg     |
                       |        tf      |
    stack pointer -->  | return 0       |
                       +----------------+
  */
  size_t* stack_frame = *esp;
  stack_frame[0] = 0;
  stack_frame[1] = (size_t)args[1];
  stack_frame[2] = (size_t)args[2];

  /* eip 指向要执行的函数地址 sf */
  *eip = args[0];

  return true;
}

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf, pthread_fun tf, void* arg) {
  struct thread *curr = thread_current();

  lock_acquire(&curr->pcb->lock);
  curr->pcb->active_thread_cnt++;
  lock_release(&curr->pcb->lock);

  /* 传递给 start_pthread 的参数 */
  void** args = malloc(4 * sizeof(void*));
  args[0] = sf;
  args[1] = tf;
  args[2] = arg;
  args[3] = curr;
  

  tid_t tid = thread_create(curr->pcb->process_name, curr->priority, start_pthread, args);
  if(tid == TID_ERROR) {
    free(args);
    goto done;
  }
  
  /* 阻塞等待子线程执行 */
  struct child_status *cs = get_child(curr->pcb, tid);
  ASSERT(cs != NULL);

  sema_down(&cs->sema);

  if(cs->exit_status == TID_ERROR) {
    goto done;
    return TID_ERROR;
  }

done:
  if(tid == TID_ERROR) {
    lock_acquire(&curr->pcb->lock);
    curr->pcb->active_thread_cnt--;
    lock_release(&curr->pcb->lock);
  }

  return tid;
}

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec) {
  void** args = exec;

  struct thread *parent = (struct thread*)args[3];
  struct thread *curr = thread_current();

  /* 设置 pcb 并切换页表 */
  curr->pcb = parent->pcb;
  process_activate();
  ASSERT(curr->pcb != NULL);

  struct intr_frame if_;
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  lock_acquire(&curr->pcb->lock);
  int idx = ++curr->pcb->next_stack_idx;
  lock_release(&curr->pcb->lock);

  /* Set up stack. */
  bool success = setup_thread(&if_.eip, &if_.esp, idx, args);
  curr->stakc_idx = idx;
  free(args);

  struct child_status* cs = get_child(curr->pcb, curr->tid);
  ASSERT(cs != NULL);

  /* 执行失败 */
  if(!success) {
    cs->exit_status = -1;
    sema_up(&cs->sema);
    thread_exit();
  }

  sema_up(&cs->sema);

  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid) {
  if(tid == TID_ERROR) {
    return TID_ERROR;
  }

  struct thread *curr = thread_current();
  struct process *pcb = curr->pcb;

  lock_acquire(&pcb->lock);
  struct child_status *cs = get_child(pcb, tid);
  if(cs == NULL || cs->active) {
    lock_release(&pcb->lock);
    return TID_ERROR;
  }
  cs->active = true;
  lock_release(&pcb->lock);

  sema_down(&cs->sema);
  return tid;
}


void pthread_exit_t(void) {
  struct thread *curr = thread_current();

  /* 释放线程对应的栈, main 线程不能释放栈帧 */
  if(!is_main_thread(curr, curr->pcb)) {
    ASSERT(curr->stakc_idx >= 1);
    uint8_t* kpage = pagedir_get_page(curr->pcb->pagedir, ((uint8_t*)PHYS_BASE) - curr->stakc_idx * PGSIZE);
    ASSERT(kpage != NULL);
    palloc_free_page(kpage);

    /* 重置页表项 */
    pagedir_clear_page(curr->pcb->pagedir, ((uint8_t*)PHYS_BASE) - curr->stakc_idx * PGSIZE);
  }

  lock_acquire(&curr->pcb->lock);
  struct child_status *cs = get_child(curr->pcb, curr->tid);
  ASSERT(cs != NULL);
  int cnt = --curr->pcb->active_thread_cnt;

  /* 存在线程在 exit 等待子线程退出, 唤醒 */
  if(curr->pcb->exit_active) {
    sema_up(&curr->pcb->exit_sema);
  }

  sema_up(&cs->sema);

  lock_release(&curr->pcb->lock);

  if(cnt == 0) {
    /* 当前不存在活跃的线程，退出进程 */
    process_exit(0);
  } else {
    curr->pcb = NULL;
    thread_exit();
  }
}


/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {
  pthread_exit_t();
}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {
  pthread_exit_t();
}

struct file_info * get_fd(struct process* pcb, int fd) {
  struct list_elem *it = list_begin(&pcb->fd_list);
  for(; it != list_end(&pcb->fd_list); it = list_next(it)) {
    struct file_info* f = list_entry(it, struct file_info, elem);
    if (f->fd == fd) {
      return f;
    }
  }
  return NULL;
}

struct child_status* get_child(struct process *pcb, tid_t tid) {
  struct list_elem* iter = list_begin(&pcb->thread_list);
  while(iter != list_end(&pcb->thread_list)) {
    struct child_status* cs = list_entry(iter, struct child_status, elem);
    if (cs->tid == tid) {
      return cs;
    }
    iter = list_next(iter);
  }
  return NULL;
}

struct pthread_synch_info* get_sync(struct process* pcb, int id) {
  struct list_elem *it = list_begin(&pcb->pthread_sync_list);
  for(; it!= list_end(&pcb->pthread_sync_list); it = list_next(it)) {
    struct pthread_synch_info* l = list_entry(it, struct pthread_synch_info, elem);
    if (l->id == id) {
      return l;
    }
  }
  return NULL;
}