#include "threads/thread.h"
#include <debug.h>
#include <list.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "float.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
// p2-prior修改：将fifo_ready_list更名为ready_list，不区分调度算法
static struct list ready_list;

// p2-alarm添加：睡眠队列
static struct list sleep_list;

//p2-mlfqs添加：
fixed_point_t load_avg;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread* idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread* initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
  void* eip;             /* Return address. */
  thread_func* function; /* Function to call. */
  void* aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

static void init_thread(struct thread*, const char* name, int priority);
static bool is_thread(struct thread*) UNUSED;
static void* alloc_frame(struct thread*, size_t size);
static void schedule(void);
static void thread_enqueue(struct thread* t);
static tid_t allocate_tid(void);
void thread_switch_tail(struct thread* prev);

static void kernel_thread(thread_func*, void* aux);
static void idle(void* aux UNUSED);
static struct thread* running_thread(void);

static struct thread* next_thread_to_run(void);
static struct thread* thread_schedule_fifo(void);
static struct thread* thread_schedule_prio(void);
static struct thread* thread_schedule_fair(void);
static struct thread* thread_schedule_mlfqs(void);
static struct thread* thread_schedule_reserved(void);

/* Determines which scheduler the kernel should use.
   Controlled by the kernel command-line options
    "-sched=fifo", "-sched=prio",
    "-sched=fair". "-sched=mlfqs"
   Is equal to SCHED_FIFO by default. */
enum sched_policy active_sched_policy;

/* Selects a thread to run from the ready list according to
   some scheduling policy, and returns a pointer to it. */
typedef struct thread* scheduler_func(void);

/* Jump table for dynamically dispatching the current scheduling
   policy in use by the kernel. */
scheduler_func* scheduler_jump_table[8] = {thread_schedule_fifo,     thread_schedule_prio,
                                           thread_schedule_fair,     thread_schedule_mlfqs,
                                           thread_schedule_reserved, thread_schedule_reserved,
                                           thread_schedule_reserved, thread_schedule_reserved};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&all_list);
  // p2-alarm: 添加睡眠队列的初始化
  list_init(&sleep_list);
  // p2-mlfqs添加：
  load_avg = fix_int(0);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
  initial_thread->nice = 0; // p2-smfs/mlfqs添加
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

// p2-alarm添加：唤醒线程
// 检查当前ticks下是否有睡眠线程应当被唤醒
void thread_wake(int64_t ticks) {
  if (list_empty(&sleep_list)) {
    return;
  }
  struct list_elem* e;
  for (e = list_begin(&sleep_list); e != list_end(&sleep_list);
  e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, elem);
    if (t->wakeup_time <= ticks) {
      e = list_remove(&t->elem)->prev; // 从睡眠队列移出去，用prev是为了在删除节点仍能正确for循环
      thread_unblock(t);
    }
  }
}

// p2-alarm添加：沉睡线程
// 使当前线程睡大觉
void thread_sleep(int64_t wakeup_time) {
  struct thread* t = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  t->wakeup_time = wakeup_time;
  t->status = THREAD_BLOCKED;
  list_push_back(&sleep_list, &t->elem);
  schedule();
  intr_set_level(old_level);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void) {
  struct thread* t = thread_current();
  // p2-mlfqs添加：
  if (active_sched_policy == SCHED_MLFQS) {
    // 每1个ticks增加一次当前线程的recent_cpu
    int t1 = timer_ticks();
    struct thread* t = thread_current();
    if (t != idle_thread) {
      t->recent_cpu = fix_add(t->recent_cpu, fix_int(1));
    }
    // 每4个ticks：更新一次所有线程的priority
    if (timer_ticks() % 4 == 0 && timer_ticks() % TIMER_FREQ != 0) { // TODO：“优先级更新必须在内核线程运行之前完成”是什么意思？
      struct list_elem* e;
      for (e = list_begin(&all_list); e != list_end(&all_list);
      e = list_next(e)) {
        struct thread* t = list_entry(e, struct thread, allelem);
        if (t == idle_thread) continue;
        t->priority = fix_trunc(fix_sub(fix_int(PRI_MAX), fix_add(
                      fix_unscale(t->recent_cpu, 4), fix_scale(fix_int(t->nice), 2))));
      }
    }
    // 每1秒：1.更新所有线程的recent_cpu；2.更新一次系统的load_avg
    if (timer_ticks() % TIMER_FREQ == 0) {
      int ready_threads = 0;
      for (struct list_elem* e = list_begin(&all_list); e != list_end(&all_list);
      e = list_next(e)) {
        struct thread* t = list_entry(e, struct thread, allelem);
        if (t == idle_thread) continue;
        if (t->status == THREAD_RUNNING || t->status == THREAD_READY) {
          ++ready_threads;
        }
        fixed_point_t load_avg_2 = fix_scale(load_avg, 2); //中间变量
        t->recent_cpu = fix_add(fix_int(t->nice), fix_mul(
                        t->recent_cpu, fix_div(load_avg_2, fix_add(load_avg_2, fix_int(1)))));
        t->priority = fix_trunc(fix_sub(fix_int(PRI_MAX), fix_add(
                      fix_unscale(t->recent_cpu, 4), fix_scale(fix_int(t->nice), 2))));
                
      }
      load_avg = fix_add(fix_mul(fix_frac(59, 60), load_avg), fix_frac(ready_threads, 60));
    }
  }

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pcb != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", idle_ticks, kernel_ticks,
         user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char* name, int priority, thread_func* function, void* aux) {
  struct thread* t;
  struct kernel_thread_frame* kf;
  struct switch_entry_frame* ef;
  struct switch_threads_frame* sf;
  tid_t tid;

  ASSERT(function != NULL);

  /* Allocate thread. */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL) {
    return TID_ERROR;
  }

  /* Initialize thread. */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();

  // 添加 thread 到父进程中
  struct thread *curr = thread_current();
  if(curr->pcb != NULL) {
    t->parent = curr;
    struct child_status* cs = malloc(sizeof(struct child_status));
    cs->child_pcb = NULL;
    cs->tid = t->tid;
    cs->active = false;
    sema_init(&cs->sema, 0);
    list_push_front(&curr->pcb->thread_list, &cs->elem);
  }
  t->nice = thread_current()->nice; //p2-mlfqs添加

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock(t);

  //p2-prior
  if (active_sched_policy != SCHED_FIFO)
    thread_yield(); //保护性yield，防止新创建的线程是最高优先级

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Places a thread on the ready structure appropriate for the
   current active scheduling policy.
   
   This function must be called with interrupts turned off. */
static void thread_enqueue(struct thread* t) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(is_thread(t));

// p2-prior添加：
  list_push_back(&ready_list, &t->elem);
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread* t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  thread_enqueue(t);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char* thread_name(void) { return thread_current()->name; }

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread* thread_current(void) {
  struct thread* t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void) { return thread_current()->tid; }

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void) {
  ASSERT(!intr_context());

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_switch_tail(). */
  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
  struct thread* cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread)
    thread_enqueue(cur);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void thread_foreach(thread_action_func* func, void* aux) {
  struct list_elem* e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) { 
  struct thread* cur = thread_current();
  switch (active_sched_policy) {
    case SCHED_FIFO:
      thread_current()->priority = new_priority; 
      break;
    default:
      if (new_priority >= cur->priority || cur->priority <= cur->base_priority) //说明此时该线程的优先级没有被贡献，可以修改。不然不许修改
        cur->priority = new_priority;
      cur->base_priority = new_priority;
      if (cur != thread_with_highest_prior(&ready_list))
        thread_yield();
      break;
    // default:
    //   PANIC("Shouldn't reach here.");
  }
}

/* Returns the current thread's priority. */
int thread_get_priority(void) { 
  return thread_current()->priority; 
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED) { 
  //p2 mlfqs实现
  ASSERT(nice >= -20 && nice <= 20);
  if (thread_current()->nice != nice) {
    thread_current()->nice = nice;
  }
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
  //p2 mlfqs实现
  return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
  //p2 mlfqs实现
  return fix_round(fix_scale(load_avg, 100));
  // return fix_round(load_avg);
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
  //p2 mlfqs实现
  return fix_round(fix_scale(thread_current()->recent_cpu, 100));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void idle(void* idle_started_ UNUSED) {
  struct semaphore* idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
    asm volatile("sti; hlt" : : : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func* function, void* aux) {
  ASSERT(function != NULL);

  intr_enable(); /* The scheduler runs with interrupts off. */
  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread* running_thread(void) {
  uint32_t* esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool is_thread(struct thread* t) { return t != NULL && t->magic == THREAD_MAGIC; }

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void init_thread(struct thread* t, const char* name, int priority) {
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t*)t + PGSIZE;
  t->priority = priority;
  t->base_priority = priority; //p2-prior添加
  
  t->recent_cpu = fix_int(0); //p2-mlfqs添加
  list_init(&t->holding_locks); //p2-prior添加
  t->pcb = NULL;
  t->magic = THREAD_MAGIC;
  t->stakc_idx = 1;

  /* init FPU */
  {
    fpu_reg_t temp_fpu_buffer;
    fpu_stenv(&temp_fpu_buffer);
    fpu_init();
    fpu_stenv(&t->fpu_buffer_);
    fpu_ldenv(&temp_fpu_buffer);
  }

  old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old_level);

  t->parent = NULL;
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void* alloc_frame(struct thread* t, size_t size) {
  /* Stack data is always allocated in word-size units. */
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* First-in first-out scheduler */
static struct thread* thread_schedule_fifo(void) {
  if (!list_empty(&ready_list)) {
    struct thread* t = list_entry(list_pop_front(&ready_list), struct thread, elem);
    return t;
  }
  else
    return idle_thread;
}

// p2-prior添加: 返回列表中优先级最高的线程
struct thread* thread_with_highest_prior(struct list* list) {
  if (!list_empty(list)) {
    struct list_elem* e;
    struct thread* t_max_prior = list_entry(
      list_begin(list), struct thread, elem);;
    for (e = list_begin(list); e != list_end(list); 
    e = list_next(e)) {
      struct thread* t = list_entry(e, struct thread, elem);
      if (t->priority > t_max_prior->priority) {
        t_max_prior = t;
      }
    }
    return t_max_prior;
  }
  else 
    return NULL;
}

/* Strict priority scheduler */
// p2-prior添加: 返回优先队列中优先级最高的线程
// TODO: 可以尝试在向列表插入线程时按顺序插入，调度时直接取最前\最后一个。
static struct thread* thread_schedule_prio(void) {
  struct thread* t_max_prior = thread_with_highest_prior(&ready_list);
  if (t_max_prior != NULL) {
    list_remove(&t_max_prior->elem);
    return t_max_prior;
  }
  else
    return idle_thread;
}

/* Fair priority scheduler */
static struct thread* thread_schedule_fair(void) {
  return thread_schedule_prio();
}

/* Multi-level feedback queue scheduler */
static struct thread* thread_schedule_mlfqs(void) {
  return thread_schedule_prio();
}

/* Not an actual scheduling policy — placeholder for empty
 * slots in the scheduler jump table. */
static struct thread* thread_schedule_reserved(void) {
  PANIC("Invalid scheduler policy value: %d", active_sched_policy);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread* next_thread_to_run(void) {
  return (scheduler_jump_table[active_sched_policy])();
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_switch() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void thread_switch_tail(struct thread* prev) {
  struct thread* cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

/* Schedules a new thread.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_switch_tail()
   has completed. */
static void schedule(void) {
  struct thread* cur = running_thread();
  struct thread* next = next_thread_to_run();
  struct thread* prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next)
    prev = switch_threads(cur, next);
  thread_switch_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);
uint32_t thread_fpu_ofs = offsetof(struct thread, fpu_buffer_);