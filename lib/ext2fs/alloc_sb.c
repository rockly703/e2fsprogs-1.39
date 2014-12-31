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

//��block bitmap����λsb,gdt,reserved gdt��Ӧ��bit
int ext2fs_reserve_super_and_bgd(ext2_filsys fs, 
				 dgrp_t group,
				 ext2fs_block_bitmap bmap)
{
	/* 
	 * super_blk: ��¼��ǰgroup��sb��fs�е�λ��
	 * old_desc_blk:��¼����group desc��block��fs�е�λ��
	*/
	blk_t	super_blk, old_desc_blk, new_desc_blk;
	/*
	 * old_desc_blocks:��¼��ǰgroup�б���group desc��reserved group desc��block����
	 * num_blocks: 	��¼һ��group��ʣ����õ�block����
	*/
	int	j, old_desc_blocks, num_blocks;

	num_blocks = ext2fs_super_and_bgd_loc(fs, group, &super_blk, 
					      &old_desc_blk, &new_desc_blk, 0);

	if (fs->super->s_feature_incompat & EXT2_FEATURE_INCOMPAT_META_BG)
		old_desc_blocks = fs->super->s_first_meta_bg;
	else
		old_desc_blocks = 
			fs->desc_blocks + fs->super->s_reserved_gdt_blocks;

	//sbռ����һ��block,����block bitmap����Ӧ��bit
	if (super_blk || (group == 0))
		//��group 0��һ����sb,������ʱ���sb��block no����Ϊ0
		ext2fs_mark_block_bitmap(bmap, super_blk);

	if (old_desc_blk) {
		for (j=0; j < old_desc_blocks; j++)
			//��������������reserved group desc����Щblock��ռ��,��block bitmap��Ҳ��Ҫ������Ӧ��bit
			ext2fs_mark_block_bitmap(bmap, old_desc_blk + j);
	}
	if (new_desc_blk)
		//����meta_group��˵,gdtֻռ��һ��block
		ext2fs_mark_block_bitmap(bmap, new_desc_blk);

	return num_blocks;
}
