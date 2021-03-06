/*
 * profile.c -- A simple configuration file parsing "library in a file"
 * 
 * The profile library was originally written by Theodore Ts'o in 1995
 * for use in the MIT Kerberos v5 library.  It has been
 * modified/enhanced/bug-fixed over time by other members of the MIT
 * Kerberos team.  This version was originally taken from the Kerberos
 * v5 distribution, version 1.4.2, and radically simplified for use in
 * e2fsprogs.  (Support for locking for multi-threaded operations,
 * being able to modify and update the configuration file
 * programmatically, and Mac/Windows portability have been removed.
 * It has been folded into a single C source file to make it easier to
 * fold into an application program.)
 *
 * Copyright (C) 2005, 2006 by Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 * 
 * Copyright (C) 1985-2005 by the Massachusetts Institute of Technology.
 * 
 * All rights reserved.
 * 
 * Export of this software from the United States of America may require
 * a specific license from the United States Government.  It is the
 * responsibility of any person or organization contemplating export to
 * obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original MIT software.
 * M.I.T. makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 * 
 */

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <time.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#include <et/com_err.h>
#include "profile.h"
#include "prof_err.h"

#undef STAT_ONCE_PER_SECOND
#undef HAVE_STAT

/*
 * prof_int.h
 */

typedef long prf_magic_t;

/*
 * This is the structure which stores the profile information for a
 * particular configuration file.
 */
 //每一个配置文件都有一个 struct __prf_file_t
struct _prf_file_t {
	prf_magic_t	magic;
	//配置文件名
	char		*filespec;
#ifdef STAT_ONCE_PER_SECOND
	time_t		last_stat;
#endif
	time_t		timestamp; /* time tree was last updated from file */
	int		flags;	/* r/w, dirty */
	//被update的次数
	int		upd_serial; /* incremented when data changes */
	//配置文件中的tree root
	struct profile_node *root;
	struct _prf_file_t *next;
};

typedef struct _prf_file_t *prf_file_t;

/*
 * The profile flags
 */
#define PROFILE_FILE_RW		0x0001
#define PROFILE_FILE_DIRTY	0x0002

/*
 * This structure defines the high-level, user visible profile_t
 * object, which is used as a handle by users who need to query some
 * configuration file(s)
 */
struct _profile_t {
	prf_magic_t	magic;
	//所有配置文件的链表头
	prf_file_t	first_file;
};

/*
 * Used by the profile iterator in prof_get.c
 */
#define PROFILE_ITER_LIST_SECTION	0x0001
#define PROFILE_ITER_SECTIONS_ONLY	0x0002
#define PROFILE_ITER_RELATIONS_ONLY	0x0004

#define PROFILE_ITER_FINAL_SEEN		0x0100

/*
 * Check if a filespec is last in a list (NULL on UNIX, invalid FSSpec on MacOS
 */

#define	PROFILE_LAST_FILESPEC(x) (((x) == NULL) || ((x)[0] == '\0'))

struct profile_node {
	errcode_t	magic;
	//配置文件中node的名称
	char *name;
	char *value;
	int group_level;
	int final:1;		/* Indicate don't search next file */
	int deleted:1;
	struct profile_node *first_child;
	struct profile_node *parent;
	struct profile_node *next, *prev;
};

#define CHECK_MAGIC(node) \
	  if ((node)->magic != PROF_MAGIC_NODE) \
		  return PROF_MAGIC_NODE;

/* profile parser declarations */
struct parse_state {
	int	state;
	//记录section的层数
	int	group_level;
	int	line_num;
	struct profile_node *root_section;
	//记录当前正在处理的section
	struct profile_node *current_section;
};

static profile_syntax_err_cb_t	syntax_err_cb;

static errcode_t parse_line(char *line, struct parse_state *state);

#ifdef DEBUG_PROGRAM
static errcode_t profile_write_tree_file
	(struct profile_node *root, FILE *dstfile);

static errcode_t profile_write_tree_to_buffer
	(struct profile_node *root, char **buf);
#endif


static void profile_free_node
	(struct profile_node *relation);

static errcode_t profile_create_node
	(const char *name, const char *value,
		   struct profile_node **ret_node);

#ifdef DEBUG_PROGRAM
static errcode_t profile_verify_node
	(struct profile_node *node);
#endif

static errcode_t profile_add_node
	(struct profile_node *section,
		    const char *name, const char *value,
		    struct profile_node **ret_node);

static errcode_t profile_find_node
	(struct profile_node *section,
		    const char *name, const char *value,
		    int section_flag, void **state,
		    struct profile_node **node);

static errcode_t profile_node_iterator
	(void	**iter_p, struct profile_node **ret_node,
		   char **ret_name, char **ret_value);

static errcode_t profile_open_file
	(const char * file, prf_file_t *ret_prof);

static errcode_t profile_update_file
	(prf_file_t prf);

static void profile_free_file
	(prf_file_t profile);

static errcode_t profile_get_value(profile_t profile, const char *name,
				   const char *subname, const char *subsubname,
				   const char **ret_value);


/*
 * prof_init.c --- routines that manipulate the user-visible profile_t
 * 	object.
 */

static int compstr(const void *m1, const void *m2) 
{
	const char *s1 = *((const char **) m1);
	const char *s2 = *((const char **) m2);

	return strcmp(s1, s2); 
}

static void free_list(char **list)
{
    char	**cp;

    if (list == 0)
	    return;
    
    for (cp = list; *cp; cp++)
	free(*cp);
    free(list);
}

static errcode_t get_dirlist(const char *dirname, char***ret_array)
{
	DIR *dir;
	struct dirent *de;
	struct stat st;
	errcode_t retval;
	char *fn, *cp;
	char **array = 0, **new_array;
	int max = 0, num = 0;

	dir = opendir(dirname);
	if (!dir)
		return errno;

	while ((de = readdir(dir)) != NULL) {
		for (cp = de->d_name; *cp; cp++) {
			if (!isalnum(*cp) &&
			    (*cp != '-') &&
			    (*cp != '_'))
				break;
		}
		if (*cp)
			continue;
		fn = malloc(strlen(dirname) + strlen(de->d_name) + 2);
		if (!fn) {
			retval = ENOMEM;
			goto errout;
		}
		sprintf(fn, "%s/%s", dirname, de->d_name);
		if ((stat(fn, &st) < 0) || !S_ISREG(st.st_mode)) {
			free(fn);
			continue;
		}
		if (num >= max) {
			max += 10;
			new_array = realloc(array, sizeof(char *) * (max+1));
			if (!new_array) {
				retval = ENOMEM;
				goto errout;
			}
			array = new_array;
		}
		array[num++] = fn;
	}
	qsort(array, num, sizeof(char *), compstr);
	array[num++] = 0;
	*ret_array = array;
	closedir(dir);
	return 0;
errout:
	closedir(dir);
	free_list(array);
	return retval;
}

errcode_t 
profile_init(const char **files, profile_t *ret_profile)
{
	const char **fs;
	profile_t profile;
	prf_file_t  new_file, *last;
	errcode_t retval = 0;
	char **cpp, *cp, **array = 0;

	profile = malloc(sizeof(struct _profile_t));
	if (!profile)
		return ENOMEM;
	memset(profile, 0, sizeof(struct _profile_t));
	profile->magic = PROF_MAGIC_PROFILE;
	//files数组中的文件打开后会连接到以first_file为头的链表中
	last = &profile->first_file;

        /* if the filenames list is not specified return an empty profile */
        if ( files ) {
	    for (fs = files; !PROFILE_LAST_FILESPEC(*fs); fs++) {
			/*
			 * 依次读取file数组中的文件/目录,默认情况下file数组中只保存文件,
			 * get_dirlist只能读目录,所以会返回一个错误值
			 */
		retval = get_dirlist(*fs, &array);
		if (retval == 0) {
			//默认情况下不会走进来
			for (cpp = array; (cp = *cpp); cpp++) {
				retval = profile_open_file(cp, &new_file);
				if (retval == EACCES)
					continue;
				if (retval)
					goto errout;
				*last = new_file;
				last = &new_file->next;
			}
		} else if (retval != ENOTDIR)
			goto errout;

		//*files不是目录,所以会走到下面 
		retval = profile_open_file(*fs, &new_file);
		/* if this file is missing, skip to the next */
		if (retval == ENOENT || retval == EACCES) {
			continue;
		}
		if (retval)
			goto errout;
		*last = new_file;
		//所有文件通过链表形式组织起来
		last = &new_file->next;
	    }
	    /*
	     * If all the files were not found, return the appropriate error.
	     */
	    if (!profile->first_file) {
		profile_release(profile);
		return ENOENT;
	    }
	}

	free_list(array);
        *ret_profile = profile;
        return 0;
errout:
	free_list(array);
	profile_release(profile);
	return retval;
}

void 
profile_release(profile_t profile)
{
	prf_file_t	p, next;

	if (!profile || profile->magic != PROF_MAGIC_PROFILE)
		return;

	for (p = profile->first_file; p; p = next) {
		next = p->next;
		profile_free_file(p);
	}
	profile->magic = 0;
	free(profile);
}


/*
 * prof_file.c ---- routines that manipulate an individual profile file.
 */

errcode_t profile_open_file(const char * filespec,
			    prf_file_t *ret_prof)
{
	prf_file_t	prf;
	errcode_t	retval;
	char		*home_env = 0;
	unsigned int	len;
	char		*expanded_filename;

	prf = malloc(sizeof(struct _prf_file_t));
	if (!prf)
		return ENOMEM;
	memset(prf, 0, sizeof(struct _prf_file_t));
	prf->magic = PROF_MAGIC_FILE;

	len = strlen(filespec)+1;
	if (filespec[0] == '~' && filespec[1] == '/') {
		//通过环境变量获取家目录路径
		home_env = getenv("HOME");
#ifdef HAVE_PWD_H
		if (home_env == NULL) {
			//如果环境变量中家目录路径为NULL
#ifdef HAVE_GETWUID_R 
		    struct passwd *pw, pwx;
		    uid_t uid;
		    char pwbuf[BUFSIZ];

		    uid = getuid();
		    if (!getpwuid_r(uid, &pwx, pwbuf, sizeof(pwbuf), &pw)
			&& pw != NULL && pw->pw_dir[0] != 0)
			home_env = pw->pw_dir;
#else
		    struct passwd *pw;

		    pw = getpwuid(getuid());
			//通过解析/etc/passwd获取家目录路径
		    home_env = pw->pw_dir;
#endif
		}
#endif
		if (home_env)
			//如果家目录路径获取车成功
			len += strlen(home_env);
	}
	//为解析完成的路径名分配空间
	expanded_filename = malloc(len);
	if (expanded_filename == 0)
	    return errno;
	if (home_env) {
	    strcpy(expanded_filename, home_env);
		//将~/xxx/yyy中的~替换成绝对路径
	    strcat(expanded_filename, filespec+1);
	} else
	    memcpy(expanded_filename, filespec, len);

	prf->filespec = expanded_filename;

	retval = profile_update_file(prf);
	if (retval) {
		profile_free_file(prf);
		return retval;
	}

	*ret_prof = prf;
	return 0;
}

//重新解析prf->filespec
errcode_t profile_update_file(prf_file_t prf)
{
	errcode_t retval;
#ifdef HAVE_STAT
	struct stat st;
#ifdef STAT_ONCE_PER_SECOND
	time_t now;
#endif
#endif
	FILE *f;
	char buf[2048];
	struct parse_state state;

#ifdef HAVE_STAT
#ifdef STAT_ONCE_PER_SECOND
	now = time(0);
	if (now == prf->last_stat && prf->root != NULL) {
	    return 0;
	}
#endif
	//获取配置文件的状态
	if (stat(prf->filespec, &st)) {
	    retval = errno;
	    return retval;
	}
#ifdef STAT_ONCE_PER_SECOND
	prf->last_stat = now;
#endif
	if (st.st_mtime == prf->timestamp && prf->root != NULL) {
		//如果配置文件的更新时间和profile的更新时间相同,说明配置文件没有更新
	    return 0;
	}
	if (prf->root) {
		profile_free_node(prf->root);
		prf->root = 0;
	}
#else
	/*
	 * If we don't have the stat() call, assume that our in-core
	 * memory image is correct.  That is, we won't reread the
	 * profile file if it changes.
	 */
	//通过profile_init调用进来的prf->root为NULL 
	if (prf->root) {
		//如果prf->root不为空,表示配置文件的节点树已经建立完毕
	    return 0;
	}
#endif
	memset(&state, 0, sizeof(struct parse_state));
	//创建profile的根节点
	retval = profile_create_node("(root)", 0, &state.root_section);
	if (retval)
		return retval;
	errno = 0;
	f = fopen(prf->filespec, "r");
	if (f == NULL) {
		retval = errno;
		if (retval == 0)
			retval = ENOENT;
		return retval;
	}
	//准备解析配置文件,upd_serial需要++
	prf->upd_serial++;
	while (!feof(f)) {
		if (fgets(buf, sizeof(buf), f) == NULL)
			break;
		//逐行解析
		retval = parse_line(buf, &state);
		if (retval) {
			if (syntax_err_cb)
				(syntax_err_cb)(prf->filespec, retval, 
						state.line_num);
			fclose(f);
			return retval;
		}
	}
	prf->root = state.root_section;

	fclose(f);

#ifdef HAVE_STAT
	prf->timestamp = st.st_mtime;
#endif
	return 0;
}

void profile_free_file(prf_file_t prf)
{
    if (prf->root)
	profile_free_node(prf->root);
    if (prf->filespec)
	    free(prf->filespec);
    free(prf);
}

/* Begin the profile parser */

profile_syntax_err_cb_t profile_set_syntax_err_cb(profile_syntax_err_cb_t hook)
{
	profile_syntax_err_cb_t	old;

	old = syntax_err_cb;
	syntax_err_cb = hook;
	return(old);
}

//comment,包含有[]
#define STATE_INIT_COMMENT	0
//一般的行
#define STATE_STD_LINE		1
//获取左花括号
#define STATE_GET_OBRACE	2

static char *skip_over_blanks(char *cp)
{
	while (*cp && isspace((int) (*cp)))
		//略过空格符
		cp++;
	//返回非空格符
	return cp;
}

static int end_or_comment(char ch)
{
	return (ch == 0 || ch == '#' || ch == ';');
}

//找到第一个空格符或者comment
static char *skip_over_nonblanks(char *cp)
{
	while (!end_or_comment(*cp) && !isspace(*cp))
		//即不是comment也不是空格符
		cp++;
	return cp;
}

static void strip_line(char *line)
{
	char *p = line + strlen(line);
	while (p > line && (p[-1] == '\n' || p[-1] == '\r'))
		//将行尾的换行符去掉
	    *p-- = 0;
}

static void parse_quoted_string(char *str)
{
	char *to, *from;

	to = from = str;

	for (to = from = str; *from && *from != '"'; to++, from++) {
		if (*from == '\\') {
			//str中出现反斜杠,将反斜杠的字符替换成转义字符.
			from++;
			switch (*from) {
			case 'n':
				*to = '\n';
				break;
			case 't':
				*to = '\t';
				break;
			case 'b':
				*to = '\b';
				break;
			default:
				*to = *from;
			}
			continue;
		}
		/*
		 * 如果str中出现了"\n"/"\t"/"\b"的字符串,将这些字符串转成转义字符.
		 * 
		 */
		*to = *from;
	}
	*to = '\0';
}

static errcode_t parse_line(char *line, struct parse_state *state)
{
	char	*cp, ch, *tag, *value;
	char	*p;
	errcode_t retval;
	struct profile_node	*node;
	int do_subsection = 0;
	void *iter = 0;

	//记录当前正在解析的行数,从1开始
	state->line_num++;
	if (state->state == STATE_GET_OBRACE) {
		//本次解析需要获取左花括号
		cp = skip_over_blanks(line);
		if (*cp != '{')
			return PROF_MISSING_OBRACE;
		//如果cp == '{'
		state->state = STATE_STD_LINE;
		return 0;
	}
	if (state->state == STATE_INIT_COMMENT) {
		//本次解析comment,也就是"[]"标签中的内容
		if (line[0] != '[')
			return 0;
		//stanzas都是以'['开头的,
		state->state = STATE_STD_LINE;
	}

	if (*line == 0)
		//如果是空行,没有结束符,也没有换行符
		return 0;
	//将行尾的换行符去掉
	strip_line(line);
	//去掉行首的空格符
	cp = skip_over_blanks(line);
	ch = *cp;
	//';','#'是注释符,如果出现注释符和行尾,直接返回 
	if (end_or_comment(ch))
		return 0;
	if (ch == '[') {
		if (state->group_level > 0)
			//[]标签必须处于节点的顶部,也就是group_level = 0；
			return PROF_SECTION_NOTOP;
		cp++;
		//跳过空格符
		cp = skip_over_blanks(cp);
		p = strchr(cp, ']');
		if (p == NULL)
			//找不到']'
			return PROF_SECTION_SYNTAX;
		if (*cp == '"') {
			cp++;
			parse_quoted_string(cp);
		} else {
			//将']'转成0
			*p-- = '\0';
			//将尾部所有多余空格转成0
			while (isspace(*p) && (p > cp))
				*p-- = '\0';
			if (*cp == 0)
				return PROF_SECTION_SYNTAX;
		}
		//通过root_section去找/etc/mke2fs.conf中[]中的标签
		retval = profile_find_node(state->root_section, cp, 0, 1, 
					   &iter, &state->current_section);
		if (retval == PROF_NO_SECTION) {
			//如果在node tree中没有找到这个section,就需要添加section
			retval = profile_add_node(state->root_section,
						  cp, 0,
						  &state->current_section);
			if (retval)
				return retval;
		} else if (retval)
			return retval;

		/*
		 * Finish off the rest of the line.
		 */
		cp = p+1;
		if (*cp == '*') {
			state->current_section->final = 1;
			cp++;
		}
		/*
		 * Spaces or comments after ']' should not be fatal 
		 */
		cp = skip_over_blanks(cp);
		if (!end_or_comment(*cp))
			//在[]后只能有注释符';','#',或者空格和'\0',其他任何符合都是错误的
			return PROF_SECTION_SYNTAX;
		return 0;
	}
	if (ch == '}') {
		//子section中的relation处理完毕
		if (state->group_level == 0)
			return PROF_EXTRA_CBRACE;
		if (*(cp+1) == '*')
			state->current_section->final = 1;
		//回到父section,group_level需要--
		state->current_section = state->current_section->parent;
		state->group_level--;
		return 0;
	}
	/*
	 * Parse the relations
	 */
	tag = cp;
	cp = strchr(cp, '=');
	if (!cp)
		return PROF_RELATION_SYNTAX;
	if (cp == tag)
		//如果某行以'='开头
	    return PROF_RELATION_SYNTAX;
	//将'='换成'\0'
	*cp = '\0';
	if (*tag == '"') {
		tag++;
		parse_quoted_string(tag);
	} else {
		/* Look for whitespace on left-hand side.  */
		//如果一条relation的格式是"tag = xxx",到这一步就是"tag ",还需要将末尾的空格去掉
		p = skip_over_nonblanks(tag);
		if (*p)
			//将空格符或者comment替换成0
			*p++ = 0;
		//在tag后'='之前不能有任何非空格字符 
		p = skip_over_blanks(p);
		/* If we have more non-whitespace, it's an error.  */
		if (*p)
			return PROF_RELATION_SYNTAX;
	}

	//找到tag之后的value
	cp = skip_over_blanks(cp+1);
	value = cp;
	ch = value[0];
	if (ch == '"') {
		value++;
		parse_quoted_string(value);
	} else if (end_or_comment(ch)) {
		//'{'不但可以写在'='后面,也可以在'='下一行
		do_subsection++;
		state->state = STATE_GET_OBRACE;
	} else if (value[0] == '{') {
		//左花括号后不能接任何非空格字符
		cp = skip_over_blanks(value+1);
		ch = *cp;
		if (end_or_comment(ch))
			do_subsection++;
		else
			return PROF_RELATION_SYNTAX;
	} else {
		//value中不能包含任何非空字符
		cp = skip_over_nonblanks(value);
		p = skip_over_blanks(cp);
		ch = *p;
		*cp = 0;
		if (!end_or_comment(ch))
			return PROF_RELATION_SYNTAX;
	}
	if (do_subsection) {
		p = strchr(tag, '*');
		if (p)
			*p = '\0';
		//给current_section添加一个子section,并且将current_section指向新创建的section
		retval = profile_add_node(state->current_section,
					  tag, 0, &state->current_section);
		if (retval)
			return retval;
		if (p)
			state->current_section->final = 1;
		state->group_level++;
		return 0;
	}
	p = strchr(tag, '*');
	if (p)
		*p = '\0';
	//如果不是子section,那么只增加一条relation,并不会改变state->current_section
	profile_add_node(state->current_section, tag, value, &node);
	if (p)
		node->final = 1;
	return 0;
}

#ifdef DEBUG_PROGRAM
/*
 * Return TRUE if the string begins or ends with whitespace
 */
static int need_double_quotes(char *str)
{
	if (!str || !*str)
		return 0;
	if (isspace((int) (*str)) ||isspace((int) (*(str + strlen(str) - 1))))
		return 1;
	if (strchr(str, '\n') || strchr(str, '\t') || strchr(str, '\b') ||
	    strchr(str, ' ') || strchr(str, '#') || strchr(str, ';'))
		return 1;
	return 0;
}

/*
 * Output a string with double quotes, doing appropriate backquoting
 * of characters as necessary.
 */
static void output_quoted_string(char *str, void (*cb)(const char *,void *),
				 void *data)
{
	char	ch;
	char buf[2];

	cb("\"", data);
	if (!str) {
		cb("\"", data);
		return;
	}
	buf[1] = 0;
	while ((ch = *str++)) {
		switch (ch) {
		case '\\':
			cb("\\\\", data);
			break;
		case '\n':
			cb("\\n", data);
			break;
		case '\t':
			cb("\\t", data);
			break;
		case '\b':
			cb("\\b", data);
			break;
		default:
			/* This would be a lot faster if we scanned
			   forward for the next "interesting"
			   character.  */
			buf[0] = ch;
			cb(buf, data);
			break;
		}
	}
	cb("\"", data);
}

#ifndef EOL
#define EOL "\n"
#endif

/* Errors should be returned, not ignored!  */
static void dump_profile(struct profile_node *root, int level,
			 void (*cb)(const char *, void *), void *data)
{
	int i;
	struct profile_node *p;
	void *iter;
	long retval;
	
	iter = 0;
	do {
		retval = profile_find_node(root, 0, 0, 0, &iter, &p);
		if (retval)
			break;
		for (i=0; i < level; i++)
			cb("\t", data);
		if (need_double_quotes(p->name))
			output_quoted_string(p->name, cb, data);
		else
			cb(p->name, data);
		cb(" = ", data);
		if (need_double_quotes(p->value))
			output_quoted_string(p->value, cb, data);
		else
			cb(p->value, data);
		cb(EOL, data);
	} while (iter != 0);

	iter = 0;
	do {
		retval = profile_find_node(root, 0, 0, 1, &iter, &p);
		if (retval)
			break;
		if (level == 0)	{ /* [xxx] */
			cb("[", data);
			if (need_double_quotes(p->name))
				output_quoted_string(p->name, cb, data);
			else
				cb(p->name, data);
			cb("]", data);
			cb(p->final ? "*" : "", data);
			cb(EOL, data);
			dump_profile(p, level+1, cb, data);
			cb(EOL, data);
		} else { 	/* xxx = { ... } */
			for (i=0; i < level; i++)
				cb("\t", data);
			if (need_double_quotes(p->name))
				output_quoted_string(p->name, cb, data);
			else
				cb(p->name, data);
			cb(" = {", data);
			cb(EOL, data);
			dump_profile(p, level+1, cb, data);
			for (i=0; i < level; i++)
				cb("\t", data);
			cb("}", data);
			cb(p->final ? "*" : "", data);
			cb(EOL, data);
		}
	} while (iter != 0);
}

static void dump_profile_to_file_cb(const char *str, void *data)
{
	fputs(str, data);
}

errcode_t profile_write_tree_file(struct profile_node *root, FILE *dstfile)
{
	dump_profile(root, 0, dump_profile_to_file_cb, dstfile);
	return 0;
}

struct prof_buf {
	char *base;
	size_t cur, max;
	int err;
};

static void add_data_to_buffer(struct prof_buf *b, const void *d, size_t len)
{
	if (b->err)
		return;
	if (b->max - b->cur < len) {
		size_t newsize;
		char *newptr;

		newsize = b->max + (b->max >> 1) + len + 1024;
		newptr = realloc(b->base, newsize);
		if (newptr == NULL) {
			b->err = 1;
			return;
		}
		b->base = newptr;
		b->max = newsize;
	}
	memcpy(b->base + b->cur, d, len);
	b->cur += len; 		/* ignore overflow */
}

static void dump_profile_to_buffer_cb(const char *str, void *data)
{
	add_data_to_buffer((struct prof_buf *)data, str, strlen(str));
}

errcode_t profile_write_tree_to_buffer(struct profile_node *root,
				       char **buf)
{
	struct prof_buf prof_buf = { 0, 0, 0, 0 };

	dump_profile(root, 0, dump_profile_to_buffer_cb, &prof_buf);
	if (prof_buf.err) {
		*buf = NULL;
		return ENOMEM;
	}
	add_data_to_buffer(&prof_buf, "", 1); /* append nul */
	if (prof_buf.max - prof_buf.cur > (prof_buf.max >> 3)) {
		char *newptr = realloc(prof_buf.base, prof_buf.cur);
		if (newptr)
			prof_buf.base = newptr;
	}
	*buf = prof_buf.base;
	return 0;
}
#endif

/*
 * prof_tree.c --- these routines maintain the parse tree of the
 * 	config file.
 * 
 * All of the details of how the tree is stored is abstracted away in
 * this file; all of the other profile routines build, access, and
 * modify the tree via the accessor functions found in this file.
 *
 * Each node may represent either a relation or a section header.
 * 
 * A section header must have its value field set to 0, and may a one
 * or more child nodes, pointed to by first_child.
 * 
 * A relation has as its value a pointer to allocated memory
 * containing a string.  Its first_child pointer must be null.
 *
 */

/*
 * Free a node, and any children
 */
void profile_free_node(struct profile_node *node)
{
	struct profile_node *child, *next;

	if (node->magic != PROF_MAGIC_NODE)
		return;
	
	if (node->name)
		free(node->name);
	if (node->value)
		free(node->value);

	for (child=node->first_child; child; child = next) {
		next = child->next;
		profile_free_node(child);
	}
	node->magic = 0;
	
	free(node);
}

#ifndef HAVE_STRDUP
#undef strdup
#define strdup MYstrdup
static char *MYstrdup (const char *s)
{
    size_t sz = strlen(s) + 1;
    char *p = malloc(sz);
    if (p != 0)
	memcpy(p, s, sz);
    return p;
}
#endif

/*
 * Create a node
 */
errcode_t profile_create_node(const char *name, const char *value,
			      struct profile_node **ret_node)
{
	struct profile_node *new;

	new = malloc(sizeof(struct profile_node));
	if (!new)
		return ENOMEM;
	memset(new, 0, sizeof(struct profile_node));
	//为node分配名称
	new->name = strdup(name);
	if (new->name == 0) {
	    profile_free_node(new);
	    return ENOMEM;
	}
	//如果value不为NULL,就给node->name赋值,否则赋不赋值都一样,索性不赋值了
	if (value) {
		new->value = strdup(value);
		if (new->value == 0) {
		    profile_free_node(new);
		    return ENOMEM;
		}
	}
	new->magic = PROF_MAGIC_NODE;

	*ret_node = new;
	return 0;
}

/*
 * This function verifies that all of the representation invarients of
 * the profile are true.  If not, we have a programming bug somewhere,
 * probably in this file.
 */
#ifdef DEBUG_PROGRAM
errcode_t profile_verify_node(struct profile_node *node)
{
	struct profile_node *p, *last;
	errcode_t	retval;

	CHECK_MAGIC(node);

	if (node->value && node->first_child)
		return PROF_SECTION_WITH_VALUE;

	last = 0;
	for (p = node->first_child; p; last = p, p = p->next) {
		if (p->prev != last)
			return PROF_BAD_LINK_LIST;
		if (last && (last->next != p))
			return PROF_BAD_LINK_LIST;
		if (node->group_level+1 != p->group_level)
			return PROF_BAD_GROUP_LVL;
		if (p->parent != node)
			return PROF_BAD_PARENT_PTR;
		retval = profile_verify_node(p);
		if (retval)
			return retval;
	}
	return 0;
}
#endif

/*
 * Add a node to a particular section
 */
errcode_t profile_add_node(struct profile_node *section, const char *name,
			   const char *value, struct profile_node **ret_node)
{
	errcode_t retval;
	//last记录p的前一个节点
	struct profile_node *p, *last, *new;

	CHECK_MAGIC(section);

	if (section->value)
		return PROF_ADD_NOT_SECTION;

	/*
	 * Find the place to insert the new node.  We look for the
	 * place *after* the last match of the node name, since 
	 * order matters.
	 */
	for (p=section->first_child, last = 0; p; last = p, p = p->next) {
		int cmp;
		cmp = strcmp(p->name, name);
		if (cmp > 0)
			break;
	}
	retval = profile_create_node(name, value, &new);
	if (retval)
		return retval;
	new->group_level = section->group_level+1;
	new->deleted = 0;
	new->parent = section;
	//头插
	new->prev = last;
	new->next = p;
	if (p)
		p->prev = new;
	if (last)
		last->next = new;
	else
		//如果last都为空,表示section没有子节点
		section->first_child = new;
	if (ret_node)
		*ret_node = new;
	return 0;
}

/*
 * Iterate through the section, returning the nodes which match
 * the given name.  If name is NULL, then interate through all the
 * nodes in the section.  If section_flag is non-zero, only return the
 * section which matches the name; don't return relations.  If value
 * is non-NULL, then only return relations which match the requested
 * value.  (The value argument is ignored if section_flag is non-zero.)
 * 
 * The first time this routine is called, the state pointer must be
 * null.  When this profile_find_node_relation() returns, if the state
 * pointer is non-NULL, then this routine should be called again.
 * (This won't happen if section_flag is non-zero, obviously.)
 *
 */
 /*
  * 当section_flag == 1,查找匹配section,如果section_flag == 0,查找匹配relation.
  *	查找relation的时候还需要匹配value
  */
errcode_t profile_find_node(struct profile_node *section, const char *name,
			    const char *value, int section_flag, void **state,
			    struct profile_node **node)
{
	struct profile_node *p;

	CHECK_MAGIC(section);
	p = *state;
	if (p) {
		CHECK_MAGIC(p);
	} else
		p = section->first_child;
	
	for (; p; p = p->next) {
		if (name && (strcmp(p->name, name)))
			//如果p->name不匹配
			continue;
		//如果p->name和name匹配
		if (section_flag) {
			//如果section_flag不为0,p->value如果不为0,就继续查找
			if (p->value)
				continue;
		} else {
			//如果section_flag为0,需要匹配value 
			if (!p->value)
				continue;
			if (value && (strcmp(p->value, value)))
				//value不匹配
				continue;
		}
		if (p->deleted)
		    continue;
		/* A match! */
		if (node)
			*node = p;
		break;
	}
	if (p == 0) {
		//如果没有找到匹配的node 
		*state = 0;
		return section_flag ? PROF_NO_SECTION : PROF_NO_RELATION;
	}
	/*
	 * OK, we've found one match; now let's try to find another
	 * one.  This way, if we return a non-zero state pointer,
	 * there's guaranteed to be another match that's returned.
	 */
	for (p = p->next; p; p = p->next) {
		if (name && (strcmp(p->name, name)))
			continue;
		if (section_flag) {
			if (p->value)
				continue;
		} else {
			if (!p->value)
				continue;
			if (value && (strcmp(p->value, value)))
				continue;
		}
		/* A match! */
		break;
	}
	*state = p;
	return 0;
}

/*
 * This is a general-purpose iterator for returning all nodes that
 * match the specified name array.  
 */
struct profile_iterator {
	prf_magic_t		magic;
	profile_t		profile;
	int			flags;
	//记录需要查找的 name,subname,subsubname,...
	const char 		*const *names;
	//记录当前匹配的section name,这个section可以是顶层的,也可以是下层的
	const char		*name;
	prf_file_t		file;
	//配置文件被更新的次数
	int			file_serial;
	//section的级数,0,1
	int			done_idx;
	struct profile_node 	*node;
	int			num;
};

errcode_t 
profile_iterator_create(profile_t profile, const char *const *names, int flags,
			void **ret_iter)
{
	struct profile_iterator *iter;
	int	done_idx = 0;

	if (profile == 0)
		return PROF_NO_PROFILE;
	if (profile->magic != PROF_MAGIC_PROFILE)
		return PROF_MAGIC_PROFILE;
	if (!names)
		return PROF_BAD_NAMESET;
	if (!(flags & PROFILE_ITER_LIST_SECTION)) {
		if (!names[0])
			return PROF_BAD_NAMESET;
		//一般section只分两级0,1
		done_idx = 1;
	}

	if ((iter = malloc(sizeof(struct profile_iterator))) == NULL)
		return ENOMEM;

	iter->magic = PROF_MAGIC_ITERATOR;
	iter->profile = profile;
	iter->names = names;
	iter->flags = flags;
	iter->file = profile->first_file;
	iter->done_idx = done_idx;
	iter->node = 0;
	iter->num = 0;
	*ret_iter = iter;
	return 0;
}

void profile_iterator_free(void **iter_p)
{
	struct profile_iterator *iter;

	if (!iter_p)
		return;
	iter = *iter_p;
	if (!iter || iter->magic != PROF_MAGIC_ITERATOR)
		return;
	free(iter);
	*iter_p = 0;
}

/*
 * Note: the returned character strings in ret_name and ret_value
 * points to the stored character string in the parse string.  Before
 * this string value is returned to a calling application
 * (profile_node_iterator is not an exported interface), it should be
 * strdup()'ed.
 */
errcode_t profile_node_iterator(void **iter_p, struct profile_node **ret_node,
				char **ret_name, char **ret_value)
{
	struct profile_iterator 	*iter = *iter_p;
	struct profile_node 		*section, *p;
	const char			*const *cpp;
	errcode_t			retval;
	int				skip_num = 0;

	if (!iter || iter->magic != PROF_MAGIC_ITERATOR)
		return PROF_MAGIC_ITERATOR;
	if (iter->file && iter->file->magic != PROF_MAGIC_FILE)
	    return PROF_MAGIC_FILE;
	/*
	 * If the file has changed, then the node pointer is invalid,
	 * so we'll have search the file again looking for it.
	 */
	if (iter->node && (iter->file->upd_serial != iter->file_serial)) {
		iter->flags &= ~PROFILE_ITER_FINAL_SEEN;
		skip_num = iter->num;
		iter->node = 0;
	}
	if (iter->node && iter->node->magic != PROF_MAGIC_NODE) {
	    return PROF_MAGIC_NODE;
	}
get_new_file:
	if (iter->node == 0) {
		if (iter->file == 0 ||
		    (iter->flags & PROFILE_ITER_FINAL_SEEN)) {
			profile_iterator_free(iter_p);
			if (ret_node)
				*ret_node = 0;
			if (ret_name)
				*ret_name = 0;
			if (ret_value)
				*ret_value =0;
			return 0;
		}
		//检查配置文件是否有更新,如果是,则更新节点树
		if ((retval = profile_update_file(iter->file))) {
		    if (retval == ENOENT || retval == EACCES) {
			/* XXX memory leak? */
			iter->file = iter->file->next;
			skip_num = 0;
			retval = 0;
			goto get_new_file;
		    } else {
			profile_iterator_free(iter_p);
			return retval;
		    }
		}
		iter->file_serial = iter->file->upd_serial;
		/*
		 * Find the section to list if we are a LIST_SECTION,
		 * or find the containing section if not.
		 */
		section = iter->file->root;
		for (cpp = iter->names; cpp[iter->done_idx]; cpp++) {
			/* 
			 * iter->names中有4个字符串,分别是name,subname,subsubname...,
			 * 需要分别匹配
			 */
			for (p=section->first_child; p; p = p->next) {
				if (!strcmp(p->name, *cpp) && !p->value)
					//如果p->value为空的时候说明这个节点是个section,否则为relation
					break;
			}
			if (!p) {
				//没找到name
				section = 0;
				break;
			}
			section = p;
			if (p->final)
				iter->flags |= PROFILE_ITER_FINAL_SEEN;
		}
		if (!section) {
			//当前文件没有生产节点树,查找下一个文件
			iter->file = iter->file->next;
			skip_num = 0;
			goto get_new_file;
		}
		//记录当前处理的section的name
		iter->name = *cpp;
		//记录当前处理的section的第一个child section
		iter->node = section->first_child;
	}
	/*
	 * OK, now we know iter->node is set up correctly.  Let's do
	 * the search.
	 */
	for (p = iter->node; p; p = p->next) {
		if (iter->name && strcmp(p->name, iter->name))
			continue;
		if ((iter->flags & PROFILE_ITER_SECTIONS_ONLY) &&
		    p->value)
		    //section是不能有value的
			continue;
		if ((iter->flags & PROFILE_ITER_RELATIONS_ONLY) &&
		    !p->value)
		    //relation必须有value
			continue;
		if (skip_num > 0) {
			skip_num--;
			continue;
		}
		if (p->deleted)
			continue;
		break;
	}
	iter->num++;
	if (!p) {
		iter->file = iter->file->next;
		if (iter->file) {
		}
		iter->node = 0;
		skip_num = 0;
		goto get_new_file;
	}
	//走到这里说明找到了需要的节点
	if ((iter->node = p->next) == NULL)
		//如果这个节点是当前配置文件的最后一个节点,文件指针需要移到下一个文件
		iter->file = iter->file->next;
	if (ret_node)
		*ret_node = p;
	if (ret_name)
		*ret_name = p->name;
	if (ret_value)
		*ret_value = p->value;
	return 0;
}


/*
 * prof_get.c --- routines that expose the public interfaces for
 * 	querying items from the profile.
 *
 */

/*
 * This function only gets the first value from the file; it is a
 * helper function for profile_get_string, profile_get_integer, etc.
 */
errcode_t profile_get_value(profile_t profile, const char *name,
			    const char *subname, const char *subsubname,
			    const char **ret_value)
{
	errcode_t		retval;
	void			*state;
	char			*value;
	const char		*names[4];

	names[0] = name;
	names[1] = subname;
	names[2] = subsubname;
	names[3] = 0;

	if ((retval = profile_iterator_create(profile, names,
					      PROFILE_ITER_RELATIONS_ONLY,
					      &state)))
		return retval;

	//仅仅需要获取value
	if ((retval = profile_node_iterator(&state, 0, 0, &value)))
		goto cleanup;

	if (value)
		*ret_value = value;
	else
		retval = PROF_NO_RELATION;
	
cleanup:
	profile_iterator_free(&state);
	return retval;
}

errcode_t 
profile_get_string(profile_t profile, const char *name, const char *subname,
		   const char *subsubname, const char *def_val,
		   char **ret_string)
{
	const char	*value;
	errcode_t	retval;

	if (profile) {
		retval = profile_get_value(profile, name, subname, 
					   subsubname, &value);
		if (retval == PROF_NO_SECTION || retval == PROF_NO_RELATION)
			//如果没有找到需要的section或者relation,就使用默认的value
			value = def_val;
		else if (retval)
			return retval;
	} else
		value = def_val;
    
	if (value) {
		*ret_string = malloc(strlen(value)+1);
		if (*ret_string == 0)
			return ENOMEM;
		strcpy(*ret_string, value);
	} else
		*ret_string = 0;
	return 0;
}

errcode_t 
profile_get_integer(profile_t profile, const char *name, const char *subname,
		    const char *subsubname, int def_val, int *ret_int)
{
	const char	*value;
	errcode_t	retval;
	char            *end_value;
	long		ret_long;

	*ret_int = def_val;
	if (profile == 0)
		return 0;

	retval = profile_get_value(profile, name, subname, subsubname, &value);
	if (retval == PROF_NO_SECTION || retval == PROF_NO_RELATION) {
		*ret_int = def_val;
		return 0;
	} else if (retval)
		return retval;

	if (value[0] == 0)
	    /* Empty string is no good.  */
	    return PROF_BAD_INTEGER;
	errno = 0;
	ret_long = strtol (value, &end_value, 10);

	/* Overflow or underflow.  */
	if ((ret_long == LONG_MIN || ret_long == LONG_MAX) && errno != 0)
	    return PROF_BAD_INTEGER;
	/* Value outside "int" range.  */
	//不能超过整形的范围
	if ((long) (int) ret_long != ret_long)
	    return PROF_BAD_INTEGER;
	/* Garbage in string.  */
	if (end_value != value + strlen (value))
	    return PROF_BAD_INTEGER;

	*ret_int = ret_long;
	return 0;
}

static const char *const conf_yes[] = {
    "y", "yes", "true", "t", "1", "on",
    0,
};

static const char *const conf_no[] = {
    "n", "no", "false", "nil", "0", "off",
    0,
};

static errcode_t
profile_parse_boolean(const char *s, int *ret_boolean)
{
    const char *const *p;
    
    if (ret_boolean == NULL)
    	return PROF_EINVAL;

    for(p=conf_yes; *p; p++) {
		if (!strcasecmp(*p,s)) {
			*ret_boolean = 1;
	    	return 0;
		}
    }

    for(p=conf_no; *p; p++) {
		if (!strcasecmp(*p,s)) {
			*ret_boolean = 0;
			return 0;
		}
    }
	
	return PROF_BAD_BOOLEAN;
}

errcode_t 
profile_get_boolean(profile_t profile, const char *name, const char *subname,
		    const char *subsubname, int def_val, int *ret_boolean)
{
	const char	*value;
	errcode_t	retval;

	if (profile == 0) {
		*ret_boolean = def_val;
		return 0;
	}

	retval = profile_get_value(profile, name, subname, subsubname, &value);
	if (retval == PROF_NO_SECTION || retval == PROF_NO_RELATION) {
		*ret_boolean = def_val;
		return 0;
	} else if (retval)
		return retval;
   
	return profile_parse_boolean (value, ret_boolean);
}

errcode_t 
profile_iterator(void **iter_p, char **ret_name, char **ret_value)
{
	char *name, *value;
	errcode_t	retval;
	
	retval = profile_node_iterator(iter_p, 0, &name, &value);
	if (retval)
		return retval;

	if (ret_name) {
		if (name) {
			*ret_name = malloc(strlen(name)+1);
			if (!*ret_name)
				return ENOMEM;
			strcpy(*ret_name, name);
		} else
			*ret_name = 0;
	}
	if (ret_value) {
		if (value) {
			*ret_value = malloc(strlen(value)+1);
			if (!*ret_value) {
				if (ret_name) {
					free(*ret_name);
					*ret_name = 0;
				}
				return ENOMEM;
			}
			strcpy(*ret_value, value);
		} else
			*ret_value = 0;
	}
	return 0;
}

#ifdef DEBUG_PROGRAM

/*
 * test_profile.c --- testing program for the profile routine
 */

#include "argv_parse.h"
#include "profile_helpers.h"

const char *program_name = "test_profile";

#define PRINT_VALUE	1
#define PRINT_VALUES	2

static void do_cmd(profile_t profile, char **argv)
{
	errcode_t	retval;
	const char	**names, *value;
	char		**values, **cpp;
	char	*cmd;
	int		print_status;

	cmd = *(argv);
	names = (const char **) argv + 1;
	print_status = 0;
	retval = 0;
	if (cmd == 0)
		return;
	if (!strcmp(cmd, "query")) {
		retval = profile_get_values(profile, names, &values);
		print_status = PRINT_VALUES;
	} else if (!strcmp(cmd, "query1")) {
		const char *name = 0;
		const char *subname = 0;
		const char *subsubname = 0;

		name = names[0];
		if (name)
			subname = names[1];
		if (subname)
			subsubname = names[2];
		if (subsubname && names[3]) {
			fprintf(stderr, 
				"Only 3 levels are allowed with query1\n");
			retval = EINVAL;
		} else
			retval = profile_get_value(profile, name, subname, 
						   subsubname, &value);
		print_status = PRINT_VALUE;
	} else if (!strcmp(cmd, "list_sections")) {
		retval = profile_get_subsection_names(profile, names, 
						      &values);
		print_status = PRINT_VALUES;
	} else if (!strcmp(cmd, "list_relations")) {
		retval = profile_get_relation_names(profile, names, 
						    &values);
		print_status = PRINT_VALUES;
	} else if (!strcmp(cmd, "dump")) {
		retval = profile_write_tree_file
			(profile->first_file->root, stdout);
#if 0
	} else if (!strcmp(cmd, "clear")) {
		retval = profile_clear_relation(profile, names);
	} else if (!strcmp(cmd, "update")) {
		retval = profile_update_relation(profile, names+2,
						 *names, *(names+1));
#endif
	} else if (!strcmp(cmd, "verify")) {
		retval = profile_verify_node
			(profile->first_file->root);
#if 0
	} else if (!strcmp(cmd, "rename_section")) {
		retval = profile_rename_section(profile, names+1, *names);
	} else if (!strcmp(cmd, "add")) {
		value = *names;
		if (strcmp(value, "NULL") == 0)
			value = NULL;
		retval = profile_add_relation(profile, names+1, value);
	} else if (!strcmp(cmd, "flush")) {
		retval = profile_flush(profile);
#endif
	} else {
		printf("Invalid command.\n");
	}
	if (retval) {
		com_err(cmd, retval, "");
		print_status = 0;
	}
	switch (print_status) {
	case PRINT_VALUE:
		printf("%s\n", value);
		break;
	case PRINT_VALUES:
		for (cpp = values; *cpp; cpp++)
			printf("%s\n", *cpp);
		profile_free_list(values);
		break;
	}
}

static void do_batchmode(profile_t profile)
{
	int		argc, ret;
	char		**argv;
	char		buf[256];

	while (!feof(stdin)) {
		if (fgets(buf, sizeof(buf), stdin) == NULL)
			break;
		printf(">%s", buf);
		ret = argv_parse(buf, &argc, &argv);
		if (ret != 0) {
			printf("Argv_parse returned %d!\n", ret);
			continue;
		}
		do_cmd(profile, argv);
		printf("\n");
		argv_free(argv);
	}
	profile_release(profile);
	exit(0);
	
}

void syntax_err_report(const char *filename, long err, int line_num)
{
	fprintf(stderr, "Syntax error in %s, line number %d: %s\n",
		filename, line_num, error_message(err));
	exit(1);
}

int main(int argc, char **argv)
{
    profile_t	profile;
    long	retval;
    char	*cmd;
    
    if (argc < 2) {
	    fprintf(stderr, "Usage: %s filename [cmd argset]\n", program_name);
	    exit(1);
    }

    initialize_prof_error_table();

    profile_set_syntax_err_cb(syntax_err_report);
    
    retval = profile_init_path(argv[1], &profile);
    if (retval) {
	com_err(program_name, retval, "while initializing profile");
	exit(1);
    }
    cmd = *(argv+2);
    if (!cmd || !strcmp(cmd, "batch"))
	    do_batchmode(profile);
    else
	    do_cmd(profile, argv+2);
    profile_release(profile);

    return 0;
}

#endif
