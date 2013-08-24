#ifndef __CLYDEFS_MKFS_H
#define __CLYDEFS_MKFS_H
#include "clydefs.h"

int cfsfs_create(struct cfs_node_addr *fs_sb_tbl, char const * const dev_path);
int cfsfs_destroy(char const * const dev_path, struct cfs_node_addr const * const superblock_addr);

#endif //__CLYDEFS_MKFS_H
