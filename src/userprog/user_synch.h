#include "list.h"


/* Synchronization Types */
typedef char lock_t;
typedef char sema_t;

typedef struct pthread_synch_info pthread_synch_info;

enum SyuncType {
  LOCK_TYPE,
  SEMA_TYPE,
};

struct pthread_synch_info {
  struct list_elem elem;

  int id;
  enum SyuncType type; /* 当前同步类型 */
  int tid;             /* 当前持有 lock 的 thread tid, 为 0 时表示没有被持有锁 */
  int value;      /* 信号量的 V */

  struct list waiters; /* List of waiting threads. */
};

bool pthread_lock_init(lock_t* lock);
bool pthread_lock_acquire(lock_t* lock);
bool pthread_lock_release(lock_t* lock);

bool pthread_sema_init(sema_t* sema, int val);
bool pthread_sema_down(sema_t* sema);
bool pthread_sema_up(sema_t* sema);