/*
 * feature.c --- convert between features and strings
 * 
 * Copyright (C) 1999  Theodore Ts'o <tytso@mit.edu>
 * 
 * This file can be redistributed under the terms of the GNU Library General
 * Public License
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "e2p.h"

struct feature {
	int		compat;
	unsigned int	mask;
	const char	*string;
};

static struct feature feature_list[] = {
	{	E2P_FEATURE_COMPAT, EXT2_FEATURE_COMPAT_DIR_PREALLOC,
			"dir_prealloc" },
	{	E2P_FEATURE_COMPAT, EXT3_FEATURE_COMPAT_HAS_JOURNAL,
			"has_journal" },
	{	E2P_FEATURE_COMPAT, EXT2_FEATURE_COMPAT_IMAGIC_INODES,
			"imagic_inodes" },
	{	E2P_FEATURE_COMPAT, EXT2_FEATURE_COMPAT_EXT_ATTR,
			"ext_attr" },
	{	E2P_FEATURE_COMPAT, EXT2_FEATURE_COMPAT_DIR_INDEX,
			"dir_index" },
	{	E2P_FEATURE_COMPAT, EXT2_FEATURE_COMPAT_RESIZE_INODE,
			"resize_inode" },
	{	E2P_FEATURE_COMPAT, EXT2_FEATURE_COMPAT_LAZY_BG,
			"lazy_bg" },
	{	E2P_FEATURE_RO_INCOMPAT, EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER,
			"sparse_super" },
	{	E2P_FEATURE_RO_INCOMPAT, EXT2_FEATURE_RO_COMPAT_LARGE_FILE,
			"large_file" },
	{	E2P_FEATURE_INCOMPAT, EXT2_FEATURE_INCOMPAT_COMPRESSION,
			"compression" },
	{	E2P_FEATURE_INCOMPAT, EXT2_FEATURE_INCOMPAT_FILETYPE,
			"filetype" },
	{	E2P_FEATURE_INCOMPAT, EXT3_FEATURE_INCOMPAT_RECOVER,
			"needs_recovery" },
	{	E2P_FEATURE_INCOMPAT, EXT3_FEATURE_INCOMPAT_JOURNAL_DEV,
			"journal_dev" },
	{	E2P_FEATURE_INCOMPAT, EXT3_FEATURE_INCOMPAT_EXTENTS,
			"extents" },
	{	E2P_FEATURE_INCOMPAT, EXT2_FEATURE_INCOMPAT_META_BG,
			"meta_bg" },
	{	0, 0, 0 },
};

const char *e2p_feature2string(int compat, unsigned int mask)
{
	struct feature  *f;
	static char buf[20];
	char	fchar;
	int	fnum;

	for (f = feature_list; f->string; f++) {
		if ((compat == f->compat) &&
		    (mask == f->mask))
			return f->string;
	}
	switch (compat) {
	case  E2P_FEATURE_COMPAT:
		fchar = 'C';
		break;
	case E2P_FEATURE_INCOMPAT:
		fchar = 'I';
		break;
	case E2P_FEATURE_RO_INCOMPAT:
		fchar = 'R';
		break;
	default:
		fchar = '?';
		break;
	}
	for (fnum = 0; mask >>= 1; fnum++);
	sprintf(buf, "FEATURE_%c%d", fchar, fnum);
	return buf;
}

int e2p_string2feature(char *string, int *compat_type, unsigned int *mask)
{
	struct feature  *f;
	char		*eptr;
	int		num;
	//忽略大小写比较字符串
	for (f = feature_list; f->string; f++) {
		if (!strcasecmp(string, f->string)) {
			*compat_type = f->compat;
			*mask = f->mask;
			return 0;
		}
	}
	if (strncasecmp(string, "FEATURE_", 8))
		return 1;

	//支持FEATURE_c0这种写法,其中0对应于EXT2_FEATURE_COMPAT_DIR_PREALLOC
	switch (string[8]) {
	case 'c':
	case 'C':
		*compat_type = E2P_FEATURE_COMPAT;
		break;
	case 'i':
	case 'I':
		*compat_type = E2P_FEATURE_INCOMPAT;
		break;
	case 'r':
	case 'R':
		*compat_type = E2P_FEATURE_RO_INCOMPAT;
		break;
	default:
		return 1;
	}
	if (string[9] == 0)
		return 1;
	num = strtol(string+9, &eptr, 10);
	if (num > 32 || num < 0)
		return 1;
	if (*eptr)
		return 1;
	*mask = 1 << num;
	return 0;
}

static char *skip_over_blanks(char *cp)
{
	while (*cp && isspace(*cp))
		cp++;
	return cp;
}

static char *skip_over_word(char *cp)
{
	while (*cp && !isspace(*cp) && *cp != ',')
		cp++;
	return cp;
}

/*
 * Edit a feature set array as requested by the user.  The ok_array,
 * if set, allows the application to limit what features the user is
 * allowed to set or clear using this function.
 */
 //compat_array实际上是有3个元素的u32数组
int e2p_edit_feature(const char *str, __u32 *compat_array, __u32 *ok_array)
{
	char	*cp, *buf, *next;
	int	neg;
	unsigned int	mask;
	int		compat_type;

	buf = malloc(strlen(str)+1);
	if (!buf)
		return 1;
	strcpy(buf, str);
	//将space和','为分隔符,解析cp
	for (cp = buf; cp && *cp; cp = next ? next+1 : 0) {
		neg = 0;
		cp = skip_over_blanks(cp);
		next = skip_over_word(cp);
		
		if (*next == 0)
			//如果*next == NULL,也就是说在cp中没有找到空格/','等
			next = 0;
		else
			//如果*next是空格/','等
			*next = 0;

		if ((strcasecmp(cp, "none") == 0) ||
		    (strcasecmp(cp, "clear") == 0)) {
		    //清除兼容特性
			compat_array[0] = 0;
		    //清除不兼容特性
			compat_array[1] = 0;
			//清除不兼容特性
			compat_array[2] = 0;
			continue;
		}

		//支持-feature_r0,+feature_c1这种参数形式
		switch (*cp) {
		case '-':
		case '^':
			neg++;
		case '+':
			//cp指向-/^/+后的字符
			cp++;
			break;
		}
		/*
		 * cp是包含特定的字符串,compat_type中包含了特性(兼容/不兼容/只读),
		 * mask中包含了这些特性(兼容/不兼容/只读)中的具体特性
		 */
		if (e2p_string2feature(cp, &compat_type, &mask))
			return 1;
		//如果命令行中传入的特性不在ok_array中,就返回错误,说明本程序不支持
		if (ok_array && !(ok_array[compat_type] & mask))
			return 1;
		if (neg)
			//清除mask代表的特性
			compat_array[compat_type] &= ~mask;
		else
			//置位mask代表的特性
			compat_array[compat_type] |= mask;
	}
	return 0;
}

