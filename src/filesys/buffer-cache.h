#include "devices/block.h"



void init_block_cache(void);
void block_read_cache(block_sector_t sector, void* buffer);
void block_write_cache(block_sector_t sector, const void* buffer);
void block_remove_cache(block_sector_t sector);
