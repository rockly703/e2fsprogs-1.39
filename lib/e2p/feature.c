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
	//���Դ�Сд�Ƚ��ַ���
	for (f = feature_list; f->string; f++) {
		if (!strcasecmp(string, f->string)) {
			*compat_type = f->compat;
			*mask = f->mask;
			return 0;
		}
	}
	if (strncasecmp(string, "FEATURE_", 8))
		return 1;

	//֧��FEATURE_c0����д��,����0��Ӧ��EXT2_FEATURE_COMPAT_DIR_PREALLOC
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
 //compat_arrayʵ��������3��Ԫ�ص�u32����
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
	//��space��','Ϊ�ָ���,����cp
	for (cp = buf; cp && *cp; cp = next ? next+1 : 0) {
		neg = 0;
		cp = skip_over_blanks(cp);
		next = skip_over_word(cp);
		
		if (*next == 0)
			//���*next == NULL,Ҳ����˵��cp��û���ҵ��ո�/','��
			next = 0;
		else
			//���*next�ǿո�/','��
			*next = 0;

		if ((strcasecmp(cp, "none") == 0) ||
		    (strcasecmp(cp, "clear") == 0)) {
		    //�����������
			compat_array[0] = 0;
		    //�������������
			compat_array[1] = 0;
			//�������������
			compat_array[2] = 0;
			continue;
		}

		//֧��-feature_r0,+feature_c1���ֲ�����ʽ
		switch (*cp) {
		case '-':
		case '^':
			neg++;
		case '+':
			//cpָ��-/^/+����ַ�
			cp++;
			break;
		}
		/*
		 * cp�ǰ����ض����ַ���,compat_type�а���������(����/������/ֻ��),
		 * mask�а�������Щ����(����/������/ֻ��)�еľ�������
		 */
		if (e2p_string2feature(cp, &compat_type, &mask))
			return 1;
		//����������д�������Բ���ok_array��,�ͷ��ش���,˵��������֧��
		if (ok_array && !(ok_array[compat_type] & mask))
			return 1;
		if (neg)
			//���mask���������
			compat_array[compat_type] &= ~mask;
		else
			//��λmask���������
			compat_array[compat_type] |= mask;
	}
	return 0;
}

