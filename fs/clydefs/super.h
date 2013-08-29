#ifndef __CLYDEFS_SUPER_H
#define __CLYDEFS_SUPER_H
#include "clydefs.h"

int super_init(void);
void super_exit(void);

/** 
 * Increment generation to reflect some fs change which merits 
 * persisting a new version of the superblock. 
 * @param csb the cfs-specific part of the superblock 
 */ 
static void __always_inline cfssup_sb_inc_generation(struct cfs_sb *csb)
{
    spin_lock(&csb->lock_generation);
    csb->generation++;
    spin_unlock(&csb->lock_generation);
}


#endif //__CLYDEFS_SUPER_H
