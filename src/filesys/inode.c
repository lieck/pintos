#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices/block.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/off_t.h"
#include "stdbool.h"
#include "threads/malloc.h"
#include "filesys/buffer-cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* 直接索引数量 */
#define DIRECT_BLOCK_NUM 10

/* 一级索引块内可以存储的数据块数量 */
#define PRIMARY_BLOCK_NUM 128

/* 一级索引块可以存储的最大范围(512 / 4 * 512) */
#define PRIMARY_BLOCK_SIZE 65536

/* 三种类型索引块表示的最大范围 */
#define DIRECT_BLOCK_RANGE 5120
#define PRIMARY_BLOCK_RANGE 65536 + 5120
#define SECONDARY_BLOCK_RANGE 128 * PRIMARY_BLOCK_SIZE + DIRECT_BLOCK_RANGE + PRIMARY_BLOCK_RANGE

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  off_t length;                            /* File size in bytes. */
  unsigned magic;                          /* Magic number. */
  block_sector_t direct[DIRECT_BLOCK_NUM]; /* 直接位置块 */
  block_sector_t primary_index;            /* 一级索引块 */
  block_sector_t secondary_index;          /* 二级索引块 */

  uint32_t unused[114]; /* Not used. */
};

char zero_buffer[BLOCK_SECTOR_SIZE];

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

enum index_block_iter_type {
  direct_type,
  primary_type,
  secondary_type,
};

typedef struct block_iter block_iter;

/* block 迭代器 */
struct block_iter {
  struct inode* inode;

  bool write;

  int idx;         /* 当前迭代的位置 */
  int primary_idx; /* 为 secondary_type 时, 在 secondary_buffer 中的位置 */

  enum index_block_iter_type type;       /* 当前索引块的类型 */
  block_sector_t* primary_buffer;   /* 一级索引块的缓存 */
  block_sector_t* secondary_buffer; /* 二级索引块的缓存 */

  /* 上一次分配的 sector, 一次最多需要分配三个(二级索引块/一级索引块/数据块) */
  block_sector_t allocate[6];
  size_t allocate_idx;

  /* 索引块是否为脏 */
  bool root_dirty;
  bool primary_dirty;
  bool secondary_dirty;

  bool free;
};

bool init_block_iter(block_iter* iter, struct inode* inode, off_t pos, bool write);
bool iter_next(block_iter* iter);
block_sector_t iter_get_sector(block_iter* iter);
void free_block_iter(block_iter* iter, bool free_allocate);
bool allocate_sector_nonmem(block_iter* iter, block_sector_t* sector);
void free_index_block(block_sector_t* primary_buffer);
bool load_primary_block(block_iter *iter, off_t pos, bool root);
bool load_secondary_block(block_iter *iter, off_t pos);
bool is_allocate_sector(block_iter* iter, block_sector_t sector);


/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void) { list_init(&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length) {
  struct inode_disk* disk_inode = NULL;
  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    
    for(size_t i = 0; i < DIRECT_BLOCK_NUM; i++) {
      disk_inode->direct[i] = 0;
    }
    disk_inode->primary_index = 0;
    disk_inode->secondary_index = 0;

    buffer_write(sector, disk_inode);
    free(disk_inode);
    return true;
  }
  return false;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector) {
  struct list_elem* e;
  struct inode* inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL) {
    return NULL;
  }

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  buffer_read(inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL) {
    return;
  }

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      /* 释放直接索引 */
      for (size_t i = 0; i < DIRECT_BLOCK_NUM; i++) {
        if (inode->data.direct[i] != 0) {
          free_map_release(inode->data.direct[i], 1);
        }
      }

      if (inode->data.primary_index != 0) {
        // 释放一级索引
        block_sector_t* buffer = malloc(BLOCK_SECTOR_SIZE);
        if (buffer == NULL) {
          PANIC("buffer = null");
        }
        buffer_read(inode->data.primary_index, buffer);

        free_index_block(buffer);
        free_map_release(inode->data.primary_index, 1);
        free(buffer);
      }

      if (inode->data.secondary_index != 0) {
        // 释放二级索引
        block_sector_t* secondary_buffer = malloc(BLOCK_SECTOR_SIZE);
        if (secondary_buffer == NULL) {
          PANIC("buffer = null");
        }
        buffer_read(inode->data.secondary_index, secondary_buffer);

        for (size_t i = 0; i < 128; i++) {
          if (secondary_buffer[i] != 0) {
            block_sector_t* buffer = malloc(BLOCK_SECTOR_SIZE);
            if (buffer == NULL) {
              PANIC("buffer = null");
            }
            buffer_read(secondary_buffer[i], buffer);

            free_index_block(buffer);
            free_map_release(secondary_buffer[i], 1);
            free(buffer);
          }
        }

        free_map_release(inode->data.secondary_index, 1);
        free(secondary_buffer);
      }

      free_map_release(inode->sector, 1);
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t* bounce = NULL;

  block_iter* iter = malloc(sizeof(block_iter));
  if (iter == NULL || !init_block_iter(iter, inode, offset, false)) {
    return 0;
  }

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */ 
    block_sector_t sector_idx = iter_get_sector(iter);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0) {
      break;
    }

    if(sector_idx == 0) {
      /* 要读取的为 zero block */
      memcpy(buffer + bytes_read, zero_buffer, chunk_size);
    } else if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      buffer_read(sector_idx, buffer + bytes_read);
    } else {
      /* Read sector into bounce buffer, then partially copy
            into caller's buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL) {
          break;
        }
      }

      buffer_read(sector_idx, bounce);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;

    iter_next(iter);
  }

  // 预取
  block_sector_t ps = iter_get_sector(iter);
  if(ps != 0) {
    prefetching_add(ps);
  }
  
  free(bounce);
  free_block_iter(iter, false);
  free(iter);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  // printf("inode_write_at, size=%d, offset=%d\n", size, offset);

  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t* bounce = NULL;

  if (inode->deny_write_cnt) {
    return 0;
  }

  /* 初始化迭代器 */
  block_iter* iter = malloc(sizeof(block_iter));
  if (iter == NULL || !init_block_iter(iter, inode, offset, true)) {
    free_block_iter(iter, true);
    return 0;
  }

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = iter_get_sector(iter);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0) {
      break;
    }

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Write full sector directly to disk. */
      buffer_write(sector_idx, buffer + bytes_written);
    } else {
      /* We need a bounce buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL) {
          free_block_iter(iter, true);
          break;
        }
      }

      /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left) {

        if(!is_allocate_sector(iter, sector_idx)) {
          buffer_read(sector_idx, bounce);
        } else {
          memset(bounce, 0, BLOCK_SECTOR_SIZE);
        }

      } else {
        memset(bounce, 0, BLOCK_SECTOR_SIZE);
      }

      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      buffer_write(sector_idx, bounce);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;

    if(size > 0 && !iter_next(iter)) {
      break;
    }
  }
  free_block_iter(iter, false);
  free(bounce);
  free(iter);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) { return inode->data.length; }

// ------------------------------------------------------------------------------------------------- //
// --------------------------------- 迭代器操作 ----------------------------------------------------- //
// ------------------------------------------------------------------------------------------------- //

/* 初始化迭代器 */
bool init_block_iter(block_iter* iter, struct inode* inode, off_t pos, bool write) {
  memset(iter, 0, sizeof(block_iter));
  iter->write = write;
  iter->inode = inode;

  /* 直接索引块 */
  if (pos < DIRECT_BLOCK_RANGE) {
    iter->type = direct_type;
    iter->idx = pos / BLOCK_SECTOR_SIZE;

    if(iter->write && inode->data.direct[iter->idx] == 0) {
      iter->root_dirty = true;
      block_sector_t sector = 0;
      if(!allocate_sector_nonmem(iter, &sector)) {
        return false;
      }
      inode->data.direct[iter->idx] = sector;
    }
    return true;
  }

  /* 减去直接索引块的数据范围 */
  pos -= DIRECT_BLOCK_RANGE;

  iter->primary_buffer = malloc(BLOCK_SECTOR_SIZE);
  if(iter->primary_buffer == NULL) {
    return false;
  }

  if(pos < PRIMARY_BLOCK_SIZE) {
    iter->type = primary_type;
    return load_primary_block(iter, pos, true);
  } 

  pos -= PRIMARY_BLOCK_SIZE;
  iter->type = secondary_type;
  return load_secondary_block(iter, pos);
}

/* 移动迭代器到下一个位置 */
bool iter_next(block_iter* iter) {
  /* 表示上一层的写入成功, 可以重置 index */
  iter->allocate_idx = 0;
  iter->idx++;

  /* 移动直接索引到下一个位置 */
  if (iter->type == direct_type && iter->idx < DIRECT_BLOCK_NUM) {
    if (iter->write && iter->inode->data.direct[iter->idx] == 0) {
      iter->root_dirty = true;
      return allocate_sector_nonmem(iter, &iter->inode->data.direct[iter->idx]);
    }
    return true;
  }

  /* 直接索引转换为一级索引 */
  if (iter->type == direct_type && iter->idx == DIRECT_BLOCK_NUM) {
    iter->type = primary_type;

    ASSERT(iter->primary_buffer == NULL);
    iter->primary_buffer = malloc(BLOCK_SECTOR_SIZE);
    if(iter->primary_buffer == NULL) {
      return false;
    }

    return load_primary_block(iter, 0, true);
  }

  /* 一级索引转换为二级索引 */
  if (iter->type == primary_type && iter->idx == PRIMARY_BLOCK_NUM) {
    if(iter->primary_dirty) {
      buffer_write(iter->inode->data.primary_index, iter->primary_buffer);
    }
    iter->type = secondary_type;
    return load_secondary_block(iter, 0);
  }

  /* 移动二级索引 */
  if(iter->type == secondary_type && iter->idx == PRIMARY_BLOCK_NUM) {

    /* 将一级块缓存刷盘 */
    ASSERT(iter->primary_buffer != NULL);
    if(iter->primary_dirty) {
      ASSERT(iter->secondary_buffer[iter->primary_idx] != 0);
      buffer_write(iter->secondary_buffer[iter->primary_idx], iter->primary_buffer);
    }

    iter->primary_dirty = false;
    iter->primary_idx++;
    iter->idx = 0;

    if(iter->secondary_buffer[iter->idx] == 0) {
      if(iter->write) {
        allocate_sector_nonmem(iter, &iter->secondary_buffer[iter->idx]);
        iter->secondary_dirty = true;
      }
      memset(iter->primary_buffer, 0, BLOCK_SECTOR_SIZE);
    } else {
      buffer_read(iter->secondary_buffer[iter->idx], iter->primary_buffer);
    }
  }

  ASSERT(iter->type != direct_type);
  ASSERT(iter->primary_buffer != NULL);
  if(iter->write && iter->primary_buffer[iter->idx] == 0) {
    if(!allocate_sector_nonmem(iter, &iter->primary_buffer[iter->idx])) {
      return false;
    }
    iter->primary_dirty = true;
  }

  return true;
}

/* 返回当前的 sector */
block_sector_t iter_get_sector(block_iter* iter) {
  if (iter->type == direct_type) {
    return iter->inode->data.direct[iter->idx];
  }
  ASSERT(iter->primary_buffer != NULL);
  return iter->primary_buffer[iter->idx];
}

/* 分配新的 allocate, 失败会退出所分配资源 */
bool allocate_sector_nonmem(block_iter* iter, block_sector_t* sector) {
  ASSERT(*sector == 0);
  if (!free_map_allocate(1, sector)) {
    *sector = 0;
    return false;
  }
  iter->allocate[iter->allocate_idx++] = *sector;
  return true;
}

/* 释放一级索引块的数据 */
void free_index_block(block_sector_t* primary_buffer) {
  for (size_t i = 0; i < PRIMARY_BLOCK_NUM; i++) {
    if (primary_buffer[i] != 0) {
      free_map_release(primary_buffer[i], 1);
    }
  }
}

bool is_allocate_sector(block_iter* iter, block_sector_t sector) {
  for(size_t i = 0; i < iter->allocate_idx; i++) {
    if(iter->allocate[i] == sector) {
      return true;
    }
  }
  return false;
}

/* 销毁迭代器, allocate 表示是否放弃之前分配的元素 */
void free_block_iter(block_iter* iter, bool free_allocate) {
  if(iter->free) {
    return;
  }

  iter->free = true;

  if(free_allocate && iter->allocate_idx > 0) {
    size_t debug_cnt = 0;

    if(iter->type == direct_type) {
      ASSERT(is_allocate_sector(iter, iter->inode->data.direct[iter->idx]));
      iter->inode->data.direct[iter->idx] = 0;
      debug_cnt++;
    } else if(iter->type == primary_type) {
      
      if(is_allocate_sector(iter, iter->inode->data.primary_index)) {
        iter->inode->data.primary_index = 0;
        debug_cnt++;
      }

      ASSERT(is_allocate_sector(iter, iter->primary_buffer[iter->idx]));
      iter->primary_buffer[iter->idx] = 0;
      debug_cnt++;
    } else {

      if(is_allocate_sector(iter, iter->inode->data.secondary_index)) {
        iter->inode->data.secondary_index = 0;
        debug_cnt++;
      }

      if(is_allocate_sector(iter, iter->secondary_buffer[iter->primary_idx])) {
        iter->secondary_buffer[iter->primary_idx] = 0;
        debug_cnt++;
      }

      ASSERT(is_allocate_sector(iter, iter->primary_buffer[iter->idx]));
      iter->primary_buffer[iter->idx] = 0;
      debug_cnt++;
    }

    ASSERT(debug_cnt == iter->allocate_idx);
    for(size_t i = 0; i < iter->allocate_idx; i++) {
      free_map_release(iter->allocate[i], 1);
    }
  }

  /* 将脏页刷盘 */
  if(iter->root_dirty) {
    buffer_write(iter->inode->sector, &iter->inode->data);
  }

  if(iter->primary_dirty) {
    if(iter->type == primary_type) {
      buffer_write(iter->inode->data.primary_index, iter->primary_buffer);
    } else {
      buffer_write(iter->secondary_buffer[iter->primary_idx], iter->primary_buffer);
    }
  }

  if(iter->secondary_dirty) {
    buffer_write(iter->inode->data.secondary_index, iter->secondary_buffer);
  }

  free(iter->primary_buffer);
  free(iter->secondary_buffer);
}

/* 加载或分配一级索引块, 失败时必须释放所分配的资源 */
bool load_secondary_block(block_iter *iter, off_t pos) {
  struct inode *inode = iter->inode;

  /* 要求 */
  ASSERT(iter->primary_buffer != NULL);
  ASSERT(iter->secondary_buffer == NULL);

  iter->secondary_buffer = malloc(BLOCK_SECTOR_SIZE);
  if(iter->secondary_buffer == NULL) {
    return false;
  }

  block_sector_t secondary = inode->data.secondary_index;
  bool allocate_secondary = false;

  /* 加载二级索引块 */
  if (secondary == 0) {
    if(iter->write) {
      if(!allocate_sector_nonmem(iter, &secondary)) {
        return false;
      }
      allocate_secondary = true;
    }
    memset(iter->secondary_buffer, 0, BLOCK_SECTOR_SIZE);
  } else {
    buffer_read(secondary, iter->secondary_buffer);
  }

  iter->primary_idx = pos / PRIMARY_BLOCK_SIZE;
  pos -= iter->primary_idx * PRIMARY_BLOCK_SIZE;

  if(!load_primary_block(iter, pos, false)) {
    if(allocate_secondary) {
      free_map_release(secondary, 1);
    }
    free(iter->secondary_buffer);
    iter->secondary_buffer = NULL;
    return false;
  }

  if(allocate_secondary) {
    iter->inode->data.secondary_index = secondary;
    iter->root_dirty = true;
  }
  return true;
}

/* 加载或分配一级索引块, 失败时必须释放所分配的资源 */
bool load_primary_block(block_iter *iter, off_t pos, bool root) {

  /* 要求 */
  ASSERT(iter->primary_buffer != NULL);

  /* 一级索引块 */
  block_sector_t primary;
  bool allocate_primary = false;

  if(root) {
    primary = iter->inode->data.primary_index;
  } else {
    primary = iter->secondary_buffer[iter->primary_idx];
  }

  /* 分配或读取一级索引 */
  if(primary == 0) {
    if(iter->write) {
      allocate_primary = true;
      if(!allocate_sector_nonmem(iter, &primary)) {
        goto error;
      }
    }

    memset(iter->primary_buffer, 0, BLOCK_SECTOR_SIZE);
  } else {
    buffer_read(primary, iter->primary_buffer);
  }

  /* 分配数据块 */
  iter->idx = pos / BLOCK_SECTOR_SIZE;
  if(iter->primary_buffer[iter->idx] == 0 && iter->write) {
    iter->primary_dirty = true;
    if(!allocate_sector_nonmem(iter, &iter->primary_buffer[iter->idx])) {
      goto error;
    }
  }

  if(allocate_primary) {
    if(root) {
      iter->inode->data.primary_index = primary;
      iter->root_dirty = true;
    } else {
      iter->secondary_buffer[iter->primary_idx] = primary;
      iter->secondary_dirty = true;
    }
  }

  return true;
error:
  if(allocate_primary) {
    free_map_release(primary, 1);
  }
  return false;
}