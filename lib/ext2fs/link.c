/*
 * link.c --- create links in a ext2fs directory
 * 
 * Copyright (C) 1993, 1994 Theodore Ts'o.
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

#include "ext2_fs.h"
#include "ext2fs.h"

struct link_struct  {
	const char	*name;
	int		namelen;
	ext2_ino_t	inode;
	int		flags;
	//完成标志
	int		done;
	struct ext2_super_block *sb;
};	

/*
 * 对于增加一个目录这种操作来说,需要进行如下步骤:
 * 1.找到一个目录项,这个目录项的rec_len除了能容纳本身,还可以容纳需要新建的目录项
 * 2.改变这个这个目录项的rec_len,使其能够刚好容纳本身
 * 3.利用腾出来的空间新建目录项
*/
static int link_proc(struct ext2_dir_entry *dirent,
		     int	offset,
		     int	blocksize,
		     char	*buf,
		     void	*priv_data)
{
	struct link_struct *ls = (struct link_struct *) priv_data;
    //下一个目录项
	struct ext2_dir_entry *next;
	int rec_len, min_rec_len;
	int ret = 0;

	rec_len = EXT2_DIR_REC_LEN(ls->namelen);

	/*
	 * See if the following directory entry (if any) is unused;
	 * if so, absorb it into this one.
	 */
	next = (struct ext2_dir_entry *) (buf + offset + dirent->rec_len);
	if ((offset + dirent->rec_len < blocksize - 8) &&
	    (next->inode == 0) &&
	    (offset + dirent->rec_len + next->rec_len <= blocksize)) {
        /*
         * 当前目录项的偏移加上当前目录项的长度小于blocksize - 8,
         * 也就是下一个目录项的长度合法(大于8字节)并且下个目录项的inode等于0
         * (表示这个目录项未使用).这时当前目录项的长度需要加上下个目录项的长度
         */
        dirent->rec_len += next->rec_len;
		ret = DIRENT_CHANGED;
	}

	/*
	 * If the directory entry is used, see if we can split the
	 * directory entry to make room for the new name.  If so,
	 * truncate it and return.
	 */
	if (dirent->inode) {
        //当前目录项正在使用
		min_rec_len = EXT2_DIR_REC_LEN(dirent->name_len & 0xFF);
		if (dirent->rec_len < (min_rec_len + rec_len))
            /* 
             * 如果最小长度加上将要分配的目录项的长度小于这个目录项的长度,说明这个目录项可以
             * 拆分,给新分配的目录项腾出空间
             */
			return ret;
		rec_len = dirent->rec_len - min_rec_len;
        //改变当前目录项的长度
		dirent->rec_len = min_rec_len;
        //使next指向将要分配的目录项的起始位置
		next = (struct ext2_dir_entry *) (buf + offset +
						  dirent->rec_len);
		next->inode = 0;
		next->name_len = 0;
		next->rec_len = rec_len;
		return DIRENT_CHANGED;
	}

	/*
	 * If we get this far, then the directory entry is not used.
	 * See if we can fit the request entry in.  If so, do it.
	 */
    //如果当前目录项未被使用,就使用当前目录项作为新分配的目录项
	if (dirent->rec_len < rec_len)
        //当前目录项的长度小于将要分配的目录项的长度
		return ret;
	dirent->inode = ls->inode;
	dirent->name_len = ls->namelen;
	strncpy(dirent->name, ls->name, ls->namelen);
	if (ls->sb->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE)
		dirent->name_len |= (ls->flags & 0x7) << 8;

	ls->done++;
	return DIRENT_ABORT|DIRENT_CHANGED;
}

/*
 * Note: the low 3 bits of the flags field are used as the directory
 * entry filetype.
 */
#ifdef __TURBOC__
 #pragma argsused
#endif
errcode_t ext2fs_link(ext2_filsys fs, ext2_ino_t dir, const char *name, 
		      ext2_ino_t ino, int flags)
{
	errcode_t		retval;
	struct link_struct	ls;
	struct ext2_inode	inode;

	EXT2_CHECK_MAGIC(fs, EXT2_ET_MAGIC_EXT2FS_FILSYS);

	if (!(fs->flags & EXT2_FLAG_RW))
		return EXT2_ET_RO_FILSYS;

	ls.name = name;
	ls.namelen = name ? strlen(name) : 0;
	ls.inode = ino;
	ls.flags = flags;
	ls.done = 0;
	ls.sb = fs->super;

	/* 
	 * 在inode为dir的目录下遍历名称为name的目录项,并调用link_proc建立
	 * (之前必须通过ext2_lookup检查,inode为dir的目录下是否有名称为name的目录项)
	 */
	retval = ext2fs_dir_iterate(fs, dir, DIRENT_FLAG_INCLUDE_EMPTY,
				    0, link_proc, &ls);
	if (retval)
		return retval;

	if (!ls.done)
		return EXT2_ET_DIR_NO_SPACE;

	if ((retval = ext2fs_read_inode(fs, dir, &inode)) != 0)
		return retval;

	if (inode.i_flags & EXT2_INDEX_FL) {
		inode.i_flags &= ~EXT2_INDEX_FL;
		if ((retval = ext2fs_write_inode(fs, dir, &inode)) != 0)
			return retval;
	}

	return 0;
}
