#include "user_synch.h"
#include "stddef.h"
#include "syscall.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include <limits.h>
#include <list.h>
#include <stdlib.h>
#include "process.h"

void intr_thread_block(void);

bool pthread_lock_init(lock_t* lock) {
  struct thread* curr = thread_current();
  struct process* pcb = curr->pcb;

  lock_acquire(&pcb->lock);

  struct pthread_synch_info* psi = malloc(sizeof(struct pthread_synch_info));
  psi->type = LOCK_TYPE;
  psi->id = (int)lock;
  psi->tid = 0;
  psi->value = 0;
  list_init(&psi->waiters);

  list_push_front(&pcb->pthread_sync_list, &psi->elem);

  lock_release(&pcb->lock);
  return true;
}

bool pthread_lock_acquire(lock_t* lock) {
  struct thread* curr = thread_current();
  struct process* pcb = curr->pcb;

  lock_acquire(&pcb->lock);

  struct pthread_synch_info* psi = get_sync(pcb, (int)lock);

  /* 不存在锁或当前持有锁 */
  if (psi == NULL || psi->tid == curr->tid) {
    lock_release(&pcb->lock);
    return false;
  }

  if (psi->tid == 0) {
    psi->tid = curr->tid;
    lock_release(&pcb->lock);
  } else {
    list_push_back(&psi->waiters, &curr->elem);
    lock_release(&pcb->lock);

    /* 等待锁释放唤醒 */
    intr_thread_block();
  }
  return true;
}

bool pthread_lock_release(lock_t* lock) {
  struct thread* curr = thread_current();
  struct process* pcb = curr->pcb;

  lock_acquire(&pcb->lock);

  struct pthread_synch_info* psi = get_sync(pcb, (int)lock);

  /* 不存在锁或没有持有锁 */
  if (psi == NULL || psi->tid != curr->tid) {
    lock_release(&pcb->lock);
    return false;
  }

  if (!list_empty(&psi->waiters)) {
    struct thread* next = list_entry(list_pop_front(&psi->waiters), struct thread, elem);
    psi->tid = next->tid;
    lock_release(&pcb->lock);

    thread_unblock(next);
  } else {
    psi->tid = 0;
    lock_release(&pcb->lock);
  }

  return true;
}

bool pthread_sema_init(sema_t* sema, int val) {
  if (val < 0) {
    return false;
  }

  struct thread* curr = thread_current();
  struct process* pcb = curr->pcb;

  lock_acquire(&pcb->lock);

  struct pthread_synch_info* psi = malloc(sizeof(struct pthread_synch_info));
  psi->type = SEMA_TYPE;
  psi->id = (int)sema;
  psi->tid = 0;
  psi->value = val;
  list_init(&psi->waiters);

  list_push_front(&pcb->pthread_sync_list, &psi->elem);

  lock_release(&pcb->lock);
  return true;
}

bool pthread_sema_down(sema_t* sema) {
  struct thread* curr = thread_current();
  struct process* pcb = curr->pcb;

  lock_acquire(&pcb->lock);

  struct pthread_synch_info* psi = get_sync(pcb, (int)sema);
  if (psi == NULL) {
    lock_release(&pcb->lock);
    return false;
  }

  psi->value--;

  if (psi->value < 0) {
    list_push_back(&psi->waiters, &thread_current()->elem);
    lock_release(&pcb->lock);
    intr_thread_block();
  } else {
    lock_release(&pcb->lock);
  }

  return true;
}

bool pthread_sema_up(sema_t* sema) {
  struct thread* curr = thread_current();
  struct process* pcb = curr->pcb;

  lock_acquire(&pcb->lock);

  struct pthread_synch_info* psi = get_sync(pcb, (int)sema);
  if (psi == NULL) {
    lock_release(&pcb->lock);
    return false;
  }

  psi->value++;
  if (!list_empty(&psi->waiters)) {
    thread_unblock(list_entry(list_pop_front(&psi->waiters), struct thread, elem));
  }

  lock_release(&pcb->lock);

  return true;
}

void intr_thread_block(void) {
  enum intr_level old_level;
  old_level = intr_disable();

  thread_block();

  intr_set_level(old_level);
}