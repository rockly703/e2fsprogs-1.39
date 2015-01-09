/*
 * dir_iterate.c --- ext2fs directory iteration operations
 * 
 * Copyright (C) 1993, 1994, 1994, 1995, 1996, 1997 Theodore Ts'o.
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
#if HAVE_ERRNO_H
#include <errno.h>
#endif

#include "ext2_fs.h"
#include "ext2fsP.h"

/*
 * This function checks to see whether or not a potential deleted
 * directory entry looks valid.  What we do is check the deleted entry
 * and each successive entry to make sure that they all look valid and
 * that the last deleted entry ends at the beginning of the next
 * undeleted entry.  Returns 1 if the deleted entry looks valid, zero
 * if not valid.
 */
//判断一个被删除的目录项是否是合法的目录项
static int ext2fs_validate_entry(char *buf, int offset, int final_offset)
{
	struct ext2_dir_entry *dirent;
	
	while (offset < final_offset) {
        //在offset到final_offset之间可能有多个目录项
		dirent = (struct ext2_dir_entry *)(buf + offset);
		offset += dirent->rec_len;
		if ((dirent->rec_len < 8) ||
		    ((dirent->rec_len % 4) != 0) ||
		    (((dirent->name_len & 0xFF)+8) > dirent->rec_len))
            //目录项非法
			return 0;
	}
    //如果被删除的目录项的结尾和下个没有被删除的目录项开头相等,说明这些被删除的目录项是合法的
	return (offset == final_offset);
}

errcode_t ext2fs_dir_iterate2(ext2_filsys fs,
			      ext2_ino_t dir,
			      int flags,
			      char *block_buf,
			      int (*func)(ext2_ino_t	dir,
					  int		entry,
					  struct ext2_dir_entry *dirent,
					  int	offset,
					  int	blocksize,
					  char	*buf,
					  void	*priv_data),
			      void *priv_data)
{
	struct		dir_context	ctx;
	errcode_t	retval;
	
	EXT2_CHECK_MAGIC(fs, EXT2_ET_MAGIC_EXT2FS_FILSYS);

    //检查第dir个inode是否是一个目录
	retval = ext2fs_check_directory(fs, dir);
	if (retval)
		return retval;
	
	ctx.dir = dir;
	ctx.flags = flags;
	if (block_buf)
		ctx.buf = block_buf;
	else {
		retval = ext2fs_get_mem(fs->blocksize, &ctx.buf);
		if (retval)
			return retval;
	}
	ctx.func = func;
	ctx.priv_data = priv_data;
	ctx.errcode = 0;
	retval = ext2fs_block_iterate2(fs, dir, 0, 0,
				       ext2fs_process_dir_block, &ctx);
	if (!block_buf)
		ext2fs_free_mem(&ctx.buf);
	if (retval)
		return retval;
	return ctx.errcode;
}

struct xlate {
	int (*func)(struct ext2_dir_entry *dirent,
		    int		offset,
		    int		blocksize,
		    char	*buf,
		    void	*priv_data);
	void *real_private;
};

static int xlate_func(ext2_ino_t dir EXT2FS_ATTR((unused)),
		      int entry EXT2FS_ATTR((unused)),
		      struct ext2_dir_entry *dirent, int offset,
		      int blocksize, char *buf, void *priv_data)
{
	struct xlate *xl = (struct xlate *) priv_data;

	return (*xl->func)(dirent, offset, blocksize, buf, xl->real_private);
}

extern errcode_t ext2fs_dir_iterate(ext2_filsys fs, 
			      ext2_ino_t dir,
			      int flags,
			      char *block_buf,
			      int (*func)(struct ext2_dir_entry *dirent,
					  int	offset,
					  int	blocksize,
					  char	*buf,
					  void	*priv_data),
			      void *priv_data)
{
	struct xlate xl;
	
	xl.real_private = priv_data;
	xl.func = func;

	return ext2fs_dir_iterate2(fs, dir, flags, block_buf,
				   xlate_func, &xl);
}


/*
 * Helper function which is private to this module.  Used by
 * ext2fs_dir_iterate() and ext2fs_dblist_dir_iterate()
 */
int ext2fs_process_dir_block(ext2_filsys fs,
			     blk_t	*blocknr,
			     e2_blkcnt_t blockcnt,
			     blk_t	ref_block EXT2FS_ATTR((unused)),
			     int	ref_offset EXT2FS_ATTR((unused)),
			     void	*priv_data)
{
	struct dir_context *ctx = (struct dir_context *) priv_data;
    //指向下一个目录项,但这个目录项可能已经被删除
	unsigned int	offset = 0;
    //在block中遍历目录项时,指向下一个没有被删除的目录项
	unsigned int	next_real_entry = 0;
	int		ret = 0;
	int		changed = 0;
	int		do_abort = 0;
    //entry用来记录目录项的文件类型
	int		entry, size;
	struct ext2_dir_entry *dirent;

	if (blockcnt < 0)
		return 0;

    //如果blockcnt是一个目录的第一个数据块,这个数据块的第一个entry当然是'.',第二个目录项是'..'
	entry = blockcnt ? DIRENT_OTHER_FILE : DIRENT_DOT_FILE;
	
    //检查*blocknr这个数据块中所有的目录项是否合法,并且将数据块的全部内容读取到ctx->buf中去
	ctx->errcode = ext2fs_read_dir_block(fs, *blocknr, ctx->buf);
	if (ctx->errcode)
		return BLOCK_ABORT;

	while (offset < fs->blocksize) {
        //遍历一个数据块中的所有目录项
		dirent = (struct ext2_dir_entry *) (ctx->buf + offset);
		if (((offset + dirent->rec_len) > fs->blocksize) ||
		    (dirent->rec_len < 8) ||
		    ((dirent->rec_len % 4) != 0) ||
		    (((dirent->name_len & 0xFF)+8) > dirent->rec_len)) {
            //如果目录项跨了block或者目录项长度小于8,或者目录项长度没有以4对齐,或者目录名大于目录项的长度
			ctx->errcode = EXT2_ET_DIR_CORRUPTED;
			return BLOCK_ABORT;
		}
		if (!dirent->inode &&
		    !(ctx->flags & DIRENT_FLAG_INCLUDE_EMPTY))
			goto next;

		ret = (ctx->func)(ctx->dir,
				  (next_real_entry > offset) ?
				  DIRENT_DELETED_FILE : entry,
				  dirent, offset,
				  fs->blocksize, ctx->buf,
				  ctx->priv_data);
		if (entry < DIRENT_OTHER_FILE)
			entry++;
			
		if (ret & DIRENT_CHANGED)
			changed++;
		if (ret & DIRENT_ABORT) {
			do_abort++;
			break;
		}
next:		
 		if (next_real_entry == offset)
			next_real_entry += dirent->rec_len;
 
 		if (ctx->flags & DIRENT_FLAG_INCLUDE_REMOVED) {
            /* 
             * 如果一个目录项被删除,fs只会改变这个目录项上个目录项的rec_len,
             * 并且将这个目录项的inode no置为0.
             */
            /* 
             * 下面和这个函数的意思是当前目录项的name_len +8后再4个字节对齐,
             * +8是因为目录项的inode,rec_len,name_len这几个成员的大小.
             * 这里得到的size就是当前目录项的大小(下一个目录项没有被删除过的情况)
            */
			size = ((dirent->name_len & 0xFF) + 11) & ~3;

			if (dirent->rec_len != size)  {
                //如果rec_len和size不等,就说明当前目录项下个目录项被删除
				unsigned int final_offset;

                //final_offset指向下个有效的目录项
				final_offset = offset + dirent->rec_len;
                //offset指向下个目录项(被删除的)
				offset += size;
				while (offset < final_offset &&
				       !ext2fs_validate_entry(ctx->buf,
							      offset,
							      final_offset))
                    //如果没有找到合法的被删除过的目录项,继续偏移4字节查找
					offset += 4;
				continue;
			}
		}
		offset += dirent->rec_len;
	}

	if (changed) {
        //如果目录项改变就需要回写目录项所在的整块block
		ctx->errcode = ext2fs_write_dir_block(fs, *blocknr, ctx->buf);
		if (ctx->errcode)
			return BLOCK_ABORT;
	}
	if (do_abort)
		return BLOCK_ABORT;
	return 0;
}

