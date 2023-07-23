#include "devices/block.h"
#include <stdint.h>

void init_buffer_cache(void);
void buffer_read(block_sector_t sector, void* buffer, block_sector_t pf);
void buffer_write(block_sector_t sector, const void* buffer);
void buffer_remove(block_sector_t sector);
void buffer_flush_all(void);

void buffer_background_flush(int64_t curr_time);


void prefetching_add(block_sector_t sector);
void prefetching_clean(void);