#include "buffer-cache.h"
#include "devices/block.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "stdbool.h"
#include "threads/synch.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <debug.h>

#define BUFFER_CACHE_SIZE 64
#define PREFETCHING_LIST_SIZE 24

/*
 * 一种优化：使用电梯调度
 * 维护请求队列, 读写线程从请求队列中获取 block 操作
 */

/*
 * 用于缓存 buffer
 * 要求：调用 buffer_read 和 buffer_write 需要保证不会同时读写一个 sector
 */


struct buffer_info {
  block_sector_t sector;
  bool valid;         /* 是否有效 */
  bool dirty;         /* 是否为脏数据 */
  int use;            /* 使用中的线程, 最多存在两个(预取和当前读写的线程) */
  bool reference;     /* CLOCK 引用位 */
  int64_t write_time; /* 上次写入的时间 */
};

/* 缓存区信息 */
struct buffer_info buffer_info[BUFFER_CACHE_SIZE];

/* 缓存数据 */
char buffer_data[BUFFER_CACHE_SIZE][BLOCK_SECTOR_SIZE];

/* 预取队列 */
block_sector_t prefetching[PREFETCHING_LIST_SIZE];
size_t prefetching_top;
size_t prefetching_end;

struct lock buffer_lock;

size_t victim(void);
bool get_buffer_idx(block_sector_t sector, size_t* idx);

void init_buffer_cache(void) {
  lock_init(&buffer_lock);
  prefetching_top = 0;
  prefetching_end = 0;
}

/* 从缓存中读取 sectoc */
void buffer_read(block_sector_t sector, void* buffer) {
  block_read(fs_device, sector, buffer);
  return;
  
  lock_acquire(&buffer_lock);

  size_t idx;
  bool hit = get_buffer_idx(sector, &idx);
  block_sector_t old_sector;

  if (!hit) {
    idx = victim();
    old_sector = buffer_info[idx].sector;
    buffer_info[idx].sector = sector;
    buffer_info[idx].write_time = timer_ticks();
  }

  buffer_info[idx].use++;
  buffer_info[idx].reference = true;

  // 如果正在预取, 则自选等待
  while(buffer_info[idx].use > 1) {
    lock_release(&buffer_lock);
    timer_msleep(5);
    lock_acquire(&buffer_lock);
  }

  lock_release(&buffer_lock);

  if (!hit) {
    if (buffer_info[idx].dirty) {
      block_write(fs_device, old_sector, &buffer_data[idx]);
    }

    block_read(fs_device, sector, &buffer_data[idx]);
    buffer_info[idx].dirty = false;
  }

  memcpy(buffer, &buffer_data[idx], BLOCK_SECTOR_SIZE);

  lock_acquire(&buffer_lock);
  buffer_info[idx].use--;
  lock_release(&buffer_lock);
}

void buffer_write(block_sector_t sector, const void* buffer) {
  block_write(fs_device, sector, buffer);
  return;

  lock_acquire(&buffer_lock);
  prefetching_add(sector);

  size_t idx;
  bool hit = get_buffer_idx(sector, &idx);
  block_sector_t old_sector;
  if (!hit) {
    idx = victim();
    old_sector = buffer_info[idx].sector;
    buffer_info[idx].sector = sector;
    buffer_info[idx].write_time = timer_ticks();
  }

  buffer_info[idx].use++;
  buffer_info[idx].reference = true;

  // 如果正在预取, 则自旋等待
  while(buffer_info[idx].use > 1) {
    lock_release(&buffer_lock);
    timer_msleep(5);
    lock_acquire(&buffer_lock);
  }

  lock_release(&buffer_lock);

  if (!hit && buffer_info[idx].dirty) {
    block_write(fs_device, old_sector, &buffer_data[idx]);
  }

  memcpy(&buffer_data[idx], buffer, BLOCK_SECTOR_SIZE);
  block_write(fs_device, sector, buffer);
  buffer_info[idx].dirty = true;

  lock_acquire(&buffer_lock);
  buffer_info[idx].use--;
  lock_release(&buffer_lock);
}

void buffer_remove(block_sector_t sector) {
  lock_acquire(&buffer_lock);
  for (size_t i = 0; i < BUFFER_CACHE_SIZE; i++) {
    if (buffer_info[i].sector == sector) {
      buffer_info[i].valid = false;
      buffer_info[i].sector = 0;
      buffer_info[i].dirty = false;
      buffer_info[i].use = 0;
      break;
    }
  }
  lock_release(&buffer_lock);
}

/* 将所有的脏 block 刷盘 */
void buffer_flush_all(void) {
  lock_acquire(&buffer_lock);
  for (size_t i = 0; i < BUFFER_CACHE_SIZE; i++) {
    if (buffer_info[i].valid && buffer_info[i].dirty) {
      block_write(fs_device, buffer_info[i].sector, &buffer_data[i]);
      buffer_info[i].dirty = false;
    }
  }
  lock_release(&buffer_lock);
}

/* 淘汰 fream */
size_t victim(void) {
  static int idx = 0;

  int check_round = 0;
  for (;; idx = (idx + 1) % BUFFER_CACHE_SIZE) {
    if (buffer_info[idx].use > 0) {
      continue;
    }

    if (buffer_info[idx].reference) {
      buffer_info[idx].reference = false;
    } else {
      buffer_info[idx].valid = true;
      return idx;
    }

    check_round++;
    ASSERT(check_round <= 30000);
  }

  return 0;
}

/* 获取 sector 所在的位置 */
bool get_buffer_idx(block_sector_t sector, size_t* idx) {
  for (size_t i = 0; i < BUFFER_CACHE_SIZE; i++) {
    if (buffer_info[i].sector == sector) {
      *idx = i;
      return true;
    }
  }
  *idx = 0;
  return false;
}

/* 添加到预取队列, 调用需要外部同步 */
void prefetching_add(block_sector_t sector) {
  size_t next = (prefetching_top + 1) % PREFETCHING_LIST_SIZE;
  if (next == prefetching_end) {
    prefetching_end = (prefetching_top + 1) % PREFETCHING_LIST_SIZE;
  }

  prefetching[prefetching_top] = sector;
  prefetching_top = next;
}

/* 清空预取队列 */
void prefetching_clean(void) {
  lock_acquire(&buffer_lock);
  prefetching_top = prefetching_end;
  lock_release(&buffer_lock);
}

void buffer_background_flush(int64_t curr_time) {
  lock_acquire(&buffer_lock);

  // 刷盘
  for (size_t i = 0; i < BUFFER_CACHE_SIZE; i++) {
    if (buffer_info[i].valid && buffer_info[i].dirty && !buffer_info[i].use) {
      // TIMER_FREQ 约为 1s 时间
      if (curr_time - buffer_info[i].write_time >= TIMER_FREQ + TIMER_FREQ) {
        buffer_info[i].use = true;
        buffer_info[i].write_time = curr_time;
        lock_release(&buffer_lock);

        /* 刷盘 */
        block_write(fs_device, buffer_info[i].sector, &buffer_data[i]);

        lock_acquire(&buffer_lock);
        buffer_info[i].use = false;
      }
    }
  }

  // 预取
  while (prefetching_top != prefetching_end) {
    block_sector_t sector = prefetching[prefetching_end];
    prefetching_end = (prefetching_end + 1) % PREFETCHING_LIST_SIZE;

    size_t idx;
    bool hit = get_buffer_idx(sector, &idx);
    if (!hit) {
      idx = victim();
      buffer_info[idx].dirty = false;
      buffer_info[idx].reference = true;
      buffer_info[idx].use++;
      buffer_info[idx].valid = true;
      buffer_info[idx].write_time = curr_time;
      buffer_info[idx].sector = sector;
      lock_release(&buffer_lock);

      block_read(fs_device, sector, &buffer_data[idx]);

      lock_acquire(&buffer_lock);
      buffer_info[idx].use--;
    }
  }

  lock_release(&buffer_lock);
}