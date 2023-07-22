#include "buffer-cache.h"
#include "devices/block.h"
#include "filesys/filesys.h"
#include "stdbool.h"
#include "threads/synch.h"
#include <stddef.h>
#include <string.h>
#include <debug.h>

#define BUFFER_CACHE_SIZE 64

struct buffer_info {
  block_sector_t sector;
  bool dirty;        /* 是否为脏数据 */
  volatile bool use; /* 是否在使用中 */
  bool reference;    /* CLOCK 引用位 */
};

struct buffer_info buffer_info[BUFFER_CACHE_SIZE];
char buffer_data[BUFFER_CACHE_SIZE][BLOCK_SECTOR_SIZE];
struct lock buffer_lock;

size_t victim(void);

void init_block_cache(void) { lock_init(&buffer_lock); }

void block_read_cache(block_sector_t sector, void* buffer) {
  lock_acquire(&buffer_lock);
  for (size_t i = 0; i < BUFFER_CACHE_SIZE; i++) {
    if (buffer_info[i].sector == sector) {
      buffer_info[i].use = true;
      buffer_info[i].reference = true;
      lock_release(&buffer_lock);

      memcpy(buffer, &buffer_data[i], BLOCK_SECTOR_SIZE);

      barrier();
      buffer_info[i].use = false;
      return;
    }
  }

  size_t idx = victim();
  buffer_info[idx].sector = sector;
  buffer_info[idx].use = true;
  buffer_info[idx].reference = true;
  lock_release(&buffer_lock);

  if (buffer_info[idx].dirty) {
    block_write(fs_device, sector, &buffer_data[idx]);
  }

  block_read(fs_device, sector, &buffer_data[idx]);
  memcpy(&buffer[idx], buffer, BLOCK_SECTOR_SIZE);

  barrier();
  buffer_info[idx].use = false;
}

void block_write_cache(block_sector_t sector, const void* buffer) {
  lock_acquire(&buffer_lock);
  for (size_t i = 0; i < BUFFER_CACHE_SIZE; i++) {
    if (buffer_info[i].sector == sector) {
      buffer_info[i].use = true;
      buffer_info[i].reference = true;
      lock_release(&buffer_lock);

      memcpy(&buffer_data[i], buffer, BLOCK_SECTOR_SIZE);
      buffer_info[i].dirty = true;

      barrier();
      buffer_info[i].use = false;
      return;
    }
  }

  size_t idx = victim();
  buffer_info[idx].sector = sector;
  buffer_info[idx].use = true;
  buffer_info[idx].reference = true;
  lock_release(&buffer_lock);

  if (buffer_info[idx].dirty) {
    block_write(fs_device, sector, &buffer_data[idx]);
  }

  memcpy(&buffer_data[idx], buffer, BLOCK_SECTOR_SIZE);
  buffer_info[idx].dirty = true;

  barrier();
  buffer_info[idx].use = false;
}

void block_remove_cache(block_sector_t sector) {
  lock_acquire(&buffer_lock);
  for (size_t i = 0; i < BUFFER_CACHE_SIZE; i++) {
    if (buffer_info[i].sector == sector) {
      buffer_info[i].sector = 0;
      buffer_info[i].dirty = false;
      buffer_info[i].use = false;
      break;
    }
  }
  lock_release(&buffer_lock);
}

size_t victim(void) {
  static int idx = 0;

  int check_round = 0;
  for(;; idx = (idx + 1) % BUFFER_CACHE_SIZE) {
    if(buffer_info[idx].use) {
      continue;
    }

    if(buffer_info[idx].reference) {
      buffer_info[idx].reference = false; 
    } else {
      return idx;
    }

    check_round++;
    ASSERT(check_round <= 3000);
  }
  return 0;
}