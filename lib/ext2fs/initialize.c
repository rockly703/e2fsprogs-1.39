/*
 * initialize.c --- initialize a filesystem handle given superblock
 * 	parameters.  Used by mke2fs when initializing a filesystem.
 * 
 * Copyright (C) 1994, 1995, 1996 Theodore Ts'o.
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

#if defined(__linux__)    &&	defined(EXT2_OS_LINUX)
#define CREATOR_OS EXT2_OS_LINUX
#else
#if defined(__GNU__)     &&	defined(EXT2_OS_HURD)
#define CREATOR_OS EXT2_OS_HURD
#else
#if defined(__FreeBSD__) &&	defined(EXT2_OS_FREEBSD)
#define CREATOR_OS EXT2_OS_FREEBSD
#else
#if defined(LITES) 	   &&	defined(EXT2_OS_LITES)
#define CREATOR_OS EXT2_OS_LITES
#else
#define CREATOR_OS EXT2_OS_LINUX /* by default */
#endif /* defined(LITES) && defined(EXT2_OS_LITES) */
#endif /* defined(__FreeBSD__) && defined(EXT2_OS_FREEBSD) */
#endif /* defined(__GNU__)     && defined(EXT2_OS_HURD) */
#endif /* defined(__linux__)   && defined(EXT2_OS_LINUX) */
	
/*
 * Note we override the kernel include file's idea of what the default
 * check interval (never) should be.  It's a good idea to check at
 * least *occasionally*, specially since servers will never rarely get
 * to reboot, since Linux is so robust these days.  :-)
 * 
 * 180 days (six months) seems like a good value.
 */
#ifdef EXT2_DFL_CHECKINTERVAL
#undef EXT2_DFL_CHECKINTERVAL
#endif
#define EXT2_DFL_CHECKINTERVAL (86400L * 180L)

/*
 * Calculate the number of GDT blocks to reserve for online filesystem growth.
 * The absolute maximum number of GDT blocks we can reserve is determined by
 * the number of block pointers that can fit into a single block.
 */
static unsigned int calc_reserved_gdt_blocks(ext2_filsys fs)
{
	struct ext2_super_block *sb = fs->super;
	unsigned long bpg = sb->s_blocks_per_group;
    //一个block能够容纳的group descriptor的数量
	unsigned int gdpb = fs->blocksize / sizeof(struct ext2_group_desc);
	unsigned long max_blocks = 0xffffffff;
	unsigned long rsv_groups;
	unsigned int rsv_gdb;

	/* We set it at 1024x the current filesystem size, or
	 * the upper block count limit (2^32), whichever is lower.
	 */
    //fs中能预留的block count数量不能超过当前fs中block count的1024倍
	if (sb->s_blocks_count < max_blocks / 1024)
		max_blocks = sb->s_blocks_count * 1024;
    //预留的groups
	rsv_groups = (max_blocks - sb->s_first_data_block + bpg - 1) / bpg;
    //预留的group descriptor占用的block数量
	rsv_gdb = (rsv_groups + gdpb - 1) / gdpb - fs->desc_blocks;
	if (rsv_gdb > EXT2_ADDR_PER_BLOCK(sb))
		rsv_gdb = EXT2_ADDR_PER_BLOCK(sb);
#ifdef RES_GDT_DEBUG
	printf("max_blocks %lu, rsv_groups = %lu, rsv_gdb = %u\n",
	       max_blocks, rsv_groups, rsv_gdb);
#endif

	return rsv_gdb;
}

/*
 *  ext2fs_initialize的作用:
 *  1.设置sb基本参数
 *  2.微调block count和group个数
 *  3.分配fs的block/inode bitmap
 *  4.置位fs的bitmap中sb,gdt,reserved gdt所在的block
 */
errcode_t ext2fs_initialize(const char *name, int flags,
			    struct ext2_super_block *param,
			    io_manager manager, ext2_filsys *ret_fs)
{
	ext2_filsys	fs;
	errcode_t	retval;
	struct ext2_super_block *super;
	int		frags_per_block;
	unsigned int	rem;
	unsigned int	overhead = 0;
    //记录每个group开始的block位置
	blk_t		group_block;
	unsigned int	ipg;
	dgrp_t		i;
	blk_t		numblocks;
	int		rsv_gdt;
	int		io_flags;
	char		*buf;

	if (!param || !param->s_blocks_count)
		return EXT2_ET_INVALID_ARGUMENT;
	
	retval = ext2fs_get_mem(sizeof(struct struct_ext2_filsys), &fs);
	if (retval)
		return retval;
	
	memset(fs, 0, sizeof(struct struct_ext2_filsys));
	fs->magic = EXT2_ET_MAGIC_EXT2FS_FILSYS;
	fs->flags = flags | EXT2_FLAG_RW;
	fs->umask = 022;
#ifdef WORDS_BIGENDIAN
	fs->flags |= EXT2_FLAG_SWAP_BYTES;
#endif
	io_flags = IO_FLAG_RW;
	if (flags & EXT2_FLAG_EXCLUSIVE)
		io_flags |= IO_FLAG_EXCLUSIVE;
	//这一步后fs->io就已经被初始化了
	retval = manager->open(name, io_flags, &fs->io);
	if (retval)
		goto cleanup;
	fs->image_io = fs->io;
	fs->io->app_data = fs;
	//给fs->device_name分配空间
	retval = ext2fs_get_mem(strlen(name)+1, &fs->device_name);
	if (retval)
		goto cleanup;

	//fs->device_name中保存了块设备的名称
	strcpy(fs->device_name, name);
	//给sb分配空间
	retval = ext2fs_get_mem(SUPERBLOCK_SIZE, &super);
	if (retval)
		goto cleanup;
	//fs->super指向刚给sb分配的那片内存区域
	fs->super = super;

	memset(super, 0, SUPERBLOCK_SIZE);

//如果param中的成员有值,就用param中的,如果没有就用default
#define set_field(field, default) (super->field = param->field ? \
				   param->field : (default))

	super->s_magic = EXT2_SUPER_MAGIC;
	super->s_state = EXT2_VALID_FS;

	set_field(s_log_block_size, 0);	/* default blocksize: 1024 bytes */
	set_field(s_log_frag_size, 0); /* default fragsize: 1024 bytes */
    /* 
     * 如果param中没有指定s_first_data_block,就使用super->s_log_block_size ? 0 : 1.
     * 当block size == 1kb的时候,s_first_data_block == 1, 当block size > 1kb的时候,
     * s_first_data_block = 0
     */
	set_field(s_first_data_block, super->s_log_block_size ? 0 : 1);
	set_field(s_max_mnt_count, EXT2_DFL_MAX_MNT_COUNT);
	set_field(s_errors, EXT2_ERRORS_DEFAULT);
	set_field(s_feature_compat, 0);
	set_field(s_feature_incompat, 0);
	set_field(s_feature_ro_compat, 0);
	set_field(s_first_meta_bg, 0);
	if (super->s_feature_incompat & ~EXT2_LIB_FEATURE_INCOMPAT_SUPP) {
		//如果出现了兼容特性以外的bit被置位(不兼容特性)
		retval = EXT2_ET_UNSUPP_FEATURE;
		goto cleanup;
	}
	if (super->s_feature_ro_compat & ~EXT2_LIB_FEATURE_RO_COMPAT_SUPP) {
		//只读特性
		retval = EXT2_ET_RO_UNSUPP_FEATURE;
		goto cleanup;
	}

	set_field(s_rev_level, EXT2_GOOD_OLD_REV);
	if (super->s_rev_level >= EXT2_DYNAMIC_REV) {
		//如果rev大于等于1,以一个有效inode为11
		set_field(s_first_ino, EXT2_GOOD_OLD_FIRST_INO);
		//inode size = 128
		set_field(s_inode_size, EXT2_GOOD_OLD_INODE_SIZE);
	}

	set_field(s_checkinterval, EXT2_DFL_CHECKINTERVAL);
	super->s_mkfs_time = super->s_lastcheck = fs->now ? fs->now : time(NULL);

	super->s_creator_os = CREATOR_OS;

	fs->blocksize = EXT2_BLOCK_SIZE(super);
	fs->fragsize = EXT2_FRAG_SIZE(super);
	frags_per_block = fs->blocksize / fs->fragsize;

	/* default: (fs->blocksize*8) blocks/group, up to 2^16 (GDT limit) */
    //s_blocks_per_group初始化为blocksize * 8,后面会对这个值微调
	set_field(s_blocks_per_group, fs->blocksize * 8);
	if (super->s_blocks_per_group > EXT2_MAX_BLOCKS_PER_GROUP(super))
		super->s_blocks_per_group = EXT2_MAX_BLOCKS_PER_GROUP(super);
	super->s_frags_per_group = super->s_blocks_per_group * frags_per_block;
	
	super->s_blocks_count = param->s_blocks_count;
	super->s_r_blocks_count = param->s_r_blocks_count;
	if (super->s_r_blocks_count >= param->s_blocks_count) {
		retval = EXT2_ET_INVALID_ARGUMENT;
		goto cleanup;
	}

	/*
	 * If we're creating an external journal device, we don't need
	 * to bother with the rest.
	 */
	if (super->s_feature_incompat & EXT3_FEATURE_INCOMPAT_JOURNAL_DEV) {
		fs->group_desc_count = 0;
		ext2fs_mark_super_dirty(fs);
		*ret_fs = fs;
		return 0;
	}

/* 
 * 这个标签主要是用于调整block数量,group数量的,当:
 * 1.每组inode数量大于one bitmap能够容纳的inode的时候,需要恢复原有的block数量,
 *   同时减少每组的block,以至于使group数量增加.这样每组的inode数量就会减少.
 * 2.最后一个group的block数量少于overhead+50,就需要抛弃这部分的block.
 */
retry:
    //组描述符的个数
	fs->group_desc_count = (super->s_blocks_count -
				super->s_first_data_block +
				EXT2_BLOCKS_PER_GROUP(super) - 1)
		/ EXT2_BLOCKS_PER_GROUP(super);
	if (fs->group_desc_count == 0) {
		retval = EXT2_ET_TOOSMALL;
		goto cleanup;
	}
	fs->desc_blocks = (fs->group_desc_count +
			   EXT2_DESC_PER_BLOCK(super) - 1)
		/ EXT2_DESC_PER_BLOCK(super);

    /* 
     * 在命令行和配置文件均没有指定inode_ratio的情况下,默认4096个字节分配一个inode
     */
	i = fs->blocksize >= 4096 ? 1 : 4096 / fs->blocksize;
	set_field(s_inodes_count, super->s_blocks_count / i);

	/*
	 * Make sure we have at least EXT2_FIRST_INO + 1 inodes, so
	 * that we have enough inodes for the filesystem(!)
	 */
    //如果fs中的inode总数小于fs中第一个可被用户使用的inode number(rev 0中,1-11被fs使用,从12开始才能被用户使用)
	if (super->s_inodes_count < EXT2_FIRST_INODE(super)+1)
		super->s_inodes_count = EXT2_FIRST_INODE(super)+1;
	
	/*
	 * There should be at least as many inodes as the user
	 * requested.  Figure out how many inodes per group that
	 * should be.  But make sure that we don't allocate more than
	 * one bitmap's worth of inodes each group.
	 */
    //获取每组能分配到的inode数量
	ipg = (super->s_inodes_count + fs->group_desc_count - 1) /
		fs->group_desc_count;
	if (ipg > fs->blocksize * 8) {
        //分配的inode数量不能大于一个bitmap所能容纳的inode数量
		if (super->s_blocks_per_group >= 256) {
            //一个group中的block数量必须大于block bitmap中8个字节能够表示的block数量,也就是256个
			/* Try again with slightly different parameters */
            //减少block bitmap中一个字节记录的block数量,同时恢复block总数,用于增加group数量
			super->s_blocks_per_group -= 8;
			super->s_blocks_count = param->s_blocks_count;
			super->s_frags_per_group = super->s_blocks_per_group *
				frags_per_block;
			goto retry;
		} else
			return EXT2_ET_TOO_MANY_INODES;
	}

	if (ipg > (unsigned) EXT2_MAX_INODES_PER_GROUP(super))
		ipg = EXT2_MAX_INODES_PER_GROUP(super);

    //这个时候的s_inodes_per_group并没有按block size对齐
	super->s_inodes_per_group = ipg;
	if (super->s_inodes_count > ipg * fs->group_desc_count)
		super->s_inodes_count = ipg * fs->group_desc_count;

	/*
	 * Make sure the number of inodes per group completely fills
	 * the inode table blocks in the descriptor.  If not, add some
	 * additional inodes/group.  Waste not, want not...
	 */
    //inode table占用的block数量,必须以block对齐(向上对齐)
	fs->inode_blocks_per_group = (((super->s_inodes_per_group *
					EXT2_INODE_SIZE(super)) +
				       EXT2_BLOCK_SIZE(super) - 1) /
				      EXT2_BLOCK_SIZE(super));
    //inode的个数(以block 对齐后的)
	super->s_inodes_per_group = ((fs->inode_blocks_per_group *
				      EXT2_BLOCK_SIZE(super)) /
				     EXT2_INODE_SIZE(super));
	/*
	 * Finally, make sure the number of inodes per group is a
	 * multiple of 8.  This is needed to simplify the bitmap
	 * splicing code.
	 */
    //每组inode的个数也必须是8的倍数
	super->s_inodes_per_group &= ~7;
	fs->inode_blocks_per_group = (((super->s_inodes_per_group *
					EXT2_INODE_SIZE(super)) +
				       EXT2_BLOCK_SIZE(super) - 1) /
				      EXT2_BLOCK_SIZE(super));

	/*
	 * adjust inode count to reflect the adjusted inodes_per_group
	 */
	super->s_inodes_count = super->s_inodes_per_group *
		fs->group_desc_count;
	super->s_free_inodes_count = super->s_inodes_count;

	/*
	 * check the number of reserved group descriptor table blocks
	 */
	if (super->s_feature_compat & EXT2_FEATURE_COMPAT_RESIZE_INODE)
		rsv_gdt = calc_reserved_gdt_blocks(fs);
	else
		rsv_gdt = 0;
	set_field(s_reserved_gdt_blocks, rsv_gdt);
	if (super->s_reserved_gdt_blocks > EXT2_ADDR_PER_BLOCK(super)) {
		retval = EXT2_ET_RES_GDT_BLOCKS;
		goto cleanup;
	}

	/*
	 * Overhead is the number of bookkeeping blocks per group.  It
	 * includes the superblock backup, the group descriptor
	 * backups, the inode bitmap, the block bitmap, and the inode
	 * table.
	 */

    /* 
     * 下面这行代码+2是因为对于所有的group 来说overhead(也就是meta data),
     * 一定包含block bitmap,inode bitmap,inode table.而block bitmap,
     * inode bitmap一定各占用一个block
     */
	overhead = (int) (2 + fs->inode_blocks_per_group);

    //判断fs中最后一个group是否含有完整的meta data,使用closefs.c中的ext2fs_bg_has_super
	if (ext2fs_bg_has_super(fs, fs->group_desc_count - 1))
        /* 
         * 对于含有完整meta data的group来说,它们的元数据比其他group要多了一个sb, 
         * 组描述符及备份组描述符 
         */
		overhead += 1 + fs->desc_blocks + super->s_reserved_gdt_blocks;

	/* This can only happen if the user requested too many inodes */
	if (overhead > super->s_blocks_per_group)
        //如果存储meta data的block数量比一个group所能容纳的block总数还要多
		return EXT2_ET_TOO_MANY_INODES;

	/*
	 * See if the last group is big enough to support the
	 * necessary data structures.  If not, we need to get rid of
	 * it.
	 */
    //获取最后一个group的block数量
	rem = ((super->s_blocks_count - super->s_first_data_block) %
	       super->s_blocks_per_group);
	if ((fs->group_desc_count == 1) && rem && (rem < overhead))
        /* 
         * 如果fs中只存在一个group,并且这个group中的block数量小于一个group所能容纳的block数量,
         * 并且这个group中的数量小于容纳meta data的block数量
         */
		return EXT2_ET_TOOSMALL;
	if (rem && (rem < overhead+50)) {
        //将多余少量的block抛弃
		super->s_blocks_count -= rem;
		goto retry;
	}

	/*
	 * At this point we know how big the filesystem will be.  So
	 * we can do any and all allocations that depend on the block
	 * count.
	 */

	retval = ext2fs_get_mem(strlen(fs->device_name) + 80, &buf);
	if (retval)
		goto cleanup;

	sprintf(buf, "block bitmap for %s", fs->device_name);
	retval = ext2fs_allocate_block_bitmap(fs, buf, &fs->block_map);
	if (retval)
		goto cleanup;
	
	sprintf(buf, "inode bitmap for %s", fs->device_name);
	retval = ext2fs_allocate_inode_bitmap(fs, buf, &fs->inode_map);
	if (retval)
		goto cleanup;

	ext2fs_free_mem(&buf);

    //为保存group descriptor分配空间
	retval = ext2fs_get_mem((size_t) fs->desc_blocks * fs->blocksize,
				&fs->group_desc);
	if (retval)
		goto cleanup;

    //清空group descriptor
	memset(fs->group_desc, 0, (size_t) fs->desc_blocks * fs->blocksize);

	/*
	 * Reserve the superblock and group descriptors for each
	 * group, and fill in the correct group statistics for group.
	 * Note that although the block bitmap, inode bitmap, and
	 * inode table have not been allocated (and in fact won't be
	 * by this routine), they are accounted for nevertheless.
	 */
	group_block = super->s_first_data_block;
	super->s_free_blocks_count = 0;
	for (i = 0; i < fs->group_desc_count; i++) {
		numblocks = ext2fs_reserve_super_and_bgd(fs, i, fs->block_map);
        //累加每个group中出了meta data剩余可用的block数量
		super->s_free_blocks_count += numblocks;
        //统计没个group的可用block数量
		fs->group_desc[i].bg_free_blocks_count = numblocks;
		fs->group_desc[i].bg_free_inodes_count =
			fs->super->s_inodes_per_group;
		fs->group_desc[i].bg_used_dirs_count = 0;
		
		group_block += super->s_blocks_per_group;
	}
	
	ext2fs_mark_super_dirty(fs);
	ext2fs_mark_bb_dirty(fs);
	ext2fs_mark_ib_dirty(fs);
	
	io_channel_set_blksize(fs->io, fs->blocksize);

	*ret_fs = fs;
	return 0;
cleanup:
	ext2fs_free(fs);
	return retval;
}
