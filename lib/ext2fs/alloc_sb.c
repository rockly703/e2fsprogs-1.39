/*
 * alloc_sb.c --- Allocate the superblock and block group descriptors for a 
 * newly initialized filesystem.  Used by mke2fs when initializing a filesystem
 *
 * Copyright (C) 1994, 1995, 1996, 2003 Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#include <stdio.h>
#include <string.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <time.h>
#if HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "ext2_fs.h"
#include "ext2fs.h"

//在block bitmap中置位sb,gdt,reserved gdt对应的bit
int ext2fs_reserve_super_and_bgd(ext2_filsys fs, 
				 dgrp_t group,
				 ext2fs_block_bitmap bmap)
{
	/* 
	 * super_blk: 记录当前group的sb在fs中的位置
	 * old_desc_blk:记录保存group desc的block在fs中的位置
	*/
	blk_t	super_blk, old_desc_blk, new_desc_blk;
	/*
	 * old_desc_blocks:记录当前group中保存group desc及reserved group desc的block数量
	 * num_blocks: 	记录一个group中剩余可用的block个数
	*/
	int	j, old_desc_blocks, num_blocks;

	num_blocks = ext2fs_super_and_bgd_loc(fs, group, &super_blk, 
					      &old_desc_blk, &new_desc_blk, 0);

	if (fs->super->s_feature_incompat & EXT2_FEATURE_INCOMPAT_META_BG)
		old_desc_blocks = fs->super->s_first_meta_bg;
	else
		old_desc_blocks = 
			fs->desc_blocks + fs->super->s_reserved_gdt_blocks;

	//sb占用了一个block,设置block bitmap中相应的bit
	if (super_blk || (group == 0))
		//在group 0中一定有sb,但是这时候的sb的block no可能为0
		ext2fs_mark_block_bitmap(bmap, super_blk);

	if (old_desc_blk) {
		for (j=0; j < old_desc_blocks; j++)
			//保存组描述符及reserved group desc的这些block被占用,在block bitmap中也需要设置相应的bit
			ext2fs_mark_block_bitmap(bmap, old_desc_blk + j);
	}
	if (new_desc_blk)
		//对于meta_group来说,gdt只占用一个block
		ext2fs_mark_block_bitmap(bmap, new_desc_blk);

	return num_blocks;
}
