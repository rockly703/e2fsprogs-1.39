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
	//��ɱ�־
	int		done;
	struct ext2_super_block *sb;
};	

/*
 * ��������һ��Ŀ¼���ֲ�����˵,��Ҫ�������²���:
 * 1.�ҵ�һ��Ŀ¼��,���Ŀ¼���rec_len���������ɱ���,������������Ҫ�½���Ŀ¼��
 * 2.�ı�������Ŀ¼���rec_len,ʹ���ܹ��պ����ɱ���
 * 3.�����ڳ����Ŀռ��½�Ŀ¼��
*/
static int link_proc(struct ext2_dir_entry *dirent,
		     int	offset,
		     int	blocksize,
		     char	*buf,
		     void	*priv_data)
{
	struct link_struct *ls = (struct link_struct *) priv_data;
    //��һ��Ŀ¼��
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
         * ��ǰĿ¼���ƫ�Ƽ��ϵ�ǰĿ¼��ĳ���С��blocksize - 8,
         * Ҳ������һ��Ŀ¼��ĳ��ȺϷ�(����8�ֽ�)�����¸�Ŀ¼���inode����0
         * (��ʾ���Ŀ¼��δʹ��).��ʱ��ǰĿ¼��ĳ�����Ҫ�����¸�Ŀ¼��ĳ���
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
        //��ǰĿ¼������ʹ��
		min_rec_len = EXT2_DIR_REC_LEN(dirent->name_len & 0xFF);
		if (dirent->rec_len < (min_rec_len + rec_len))
            /* 
             * �����С���ȼ��Ͻ�Ҫ�����Ŀ¼��ĳ���С�����Ŀ¼��ĳ���,˵�����Ŀ¼�����
             * ���,���·����Ŀ¼���ڳ��ռ�
             */
			return ret;
		rec_len = dirent->rec_len - min_rec_len;
        //�ı䵱ǰĿ¼��ĳ���
		dirent->rec_len = min_rec_len;
        //ʹnextָ��Ҫ�����Ŀ¼�����ʼλ��
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
    //�����ǰĿ¼��δ��ʹ��,��ʹ�õ�ǰĿ¼����Ϊ�·����Ŀ¼��
	if (dirent->rec_len < rec_len)
        //��ǰĿ¼��ĳ���С�ڽ�Ҫ�����Ŀ¼��ĳ���
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
	 * ��inodeΪdir��Ŀ¼�±�������Ϊname��Ŀ¼��,������link_proc����
	 * (֮ǰ����ͨ��ext2_lookup���,inodeΪdir��Ŀ¼���Ƿ�������Ϊname��Ŀ¼��)
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
