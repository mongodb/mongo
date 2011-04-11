/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_config_init --
 *	Initialize a config handle, used to iterate through a config string.
 */
int
__wt_config_initn(WT_CONFIG *conf, const char *str, size_t len)
{
	conf->orig = conf->cur = str;
	conf->end = str + len;
	conf->depth = 0;
	conf->top = -1;
	conf->go = NULL;

	return (0);
}

/*
 * __wt_config_init --
 *	Initialize a config handle, used to iterate through a config string.
 */
int
__wt_config_init(WT_CONFIG *conf, const char *str)
{
	size_t len;

	len = (str == NULL) ? 0 : strlen(str);

	return (__wt_config_initn(conf, str, len));
}

#define	PUSH(i, t) do {							\
	if (conf->top == -1)						\
		conf->top = conf->depth;				\
	if (conf->depth == conf->top) {					\
		out->type = (t);					\
		out->str = (conf->cur + (i));				\
	}								\
} while (0)

#define	CAP(i) do {							\
	if (conf->depth == conf->top)					\
		out->len = (size_t)((conf->cur + (i) + 1) - out->str);	\
} while (0)

typedef enum {
	A_LOOP, A_BAD, A_DOWN, A_UP, A_VALUE, A_NEXT, A_QDOWN, A_QUP,
	A_ESC, A_UNESC, A_BARE, A_NUMBARE, A_UNBARE, A_UTF8_2,
	A_UTF8_3, A_UTF8_4, A_UTF_CONTINUE
} CONFIG_ACTION;

/*
 * static void *gostruct[] = {
 *		[0 ... 255] = &&l_bad,
 *		['\t'] = &&l_loop, [' '] = &&l_loop,
 *		['\r'] = &&l_loop, ['\n'] = &&l_loop,
 *		['"'] = &&l_qup,
 *		[':'] = &&l_value, ['='] = &&l_value,
 *		[','] = &&l_next,
 *		// tracking [] and {} individually would allow fuller
 *		// validation but is really messy
 *		['('] = &&l_up, [')'] = &&l_down,
 *		['['] = &&l_up, [']'] = &&l_down,
 *		['{'] = &&l_up, ['}'] = &&l_down,
 *		// bare identifiers
 *		['-'] = &&l_numbare,
 *		['0' ... '9'] = &&l_numbare,
 *		['_'] = &&l_bare,
 *		['A' ... 'Z'] = &&l_bare, ['a' ... 'z'] = &&l_bare,
 *	};
 */
static int8_t gostruct[256] = {
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_LOOP, A_LOOP, A_BAD, A_BAD, A_LOOP, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_LOOP, A_BAD, A_QUP,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_UP, A_DOWN, A_BAD, A_BAD,
	A_NEXT, A_NUMBARE, A_BAD, A_BAD, A_NUMBARE, A_NUMBARE,
	A_NUMBARE, A_NUMBARE, A_NUMBARE, A_NUMBARE, A_NUMBARE,
	A_NUMBARE, A_NUMBARE, A_NUMBARE, A_VALUE, A_BAD, A_BAD,
	A_VALUE, A_BAD, A_BAD, A_BAD, A_BARE, A_BARE, A_BARE, A_BARE,
	A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE,
	A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE,
	A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_UP, A_BAD,
	A_DOWN, A_BAD, A_BARE, A_BAD, A_BARE, A_BARE, A_BARE, A_BARE,
	A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE,
	A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE,
	A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_BARE, A_UP, A_BAD,
	A_DOWN, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD
};

/*
 *	static void *gobare[] =
 *	{
 *		[0 ... 31] = &&l_bad,
 *		// could be more pedantic/validation-checking
 *		[32 ... 126] = &&l_loop,
 *		['\t'] = &&l_unbare, [' '] = &&l_unbare,
 *		['\r'] = &&l_unbare, ['\n'] = &&l_unbare,
 *		[':'] = &&l_unbare, ['='] = &&l_unbare,
 *		[','] = &&l_unbare,
 *		[')'] = &&l_unbare, [']'] = &&l_unbare, ['}'] = &&l_unbare,
 *		[127 ... 255] = &&l_bad
 *	};
 */
static int8_t gobare[256] = {
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_UNBARE, A_UNBARE, A_BAD, A_BAD, A_UNBARE, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_UNBARE,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_UNBARE, A_LOOP, A_LOOP, A_UNBARE, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_UNBARE, A_LOOP, A_LOOP, A_UNBARE, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_UNBARE,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_UNBARE, A_LOOP, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD
};

/*
 *	static void *gostring[] =
 *	{
 *		[0 ... 31] = &&l_bad, [127] = &&l_bad,
 *		[32 ... 126] = &&l_loop,
 *		['\\'] = &&l_esc, ['"'] = &&l_qdown,
 *		[128 ... 191] = &&l_bad,
 *		[192 ... 223] = &&l_utf8_2,
 *		[224 ... 239] = &&l_utf8_3,
 *		[240 ... 247] = &&l_utf8_4,
 *		[248 ... 255] = &&l_bad
 *	};
 */
static int8_t gostring[256] = {
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_LOOP, A_LOOP, A_QDOWN,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_ESC, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_LOOP,
	A_LOOP, A_LOOP, A_LOOP, A_LOOP, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_UTF8_2,
	A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2,
	A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2,
	A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2,
	A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2,
	A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2, A_UTF8_2,
	A_UTF8_2, A_UTF8_3, A_UTF8_3, A_UTF8_3, A_UTF8_3, A_UTF8_3,
	A_UTF8_3, A_UTF8_3, A_UTF8_3, A_UTF8_3, A_UTF8_3, A_UTF8_3,
	A_UTF8_3, A_UTF8_3, A_UTF8_3, A_UTF8_3, A_UTF8_3, A_UTF8_4,
	A_UTF8_4, A_UTF8_4, A_UTF8_4, A_UTF8_4, A_UTF8_4, A_UTF8_4,
	A_UTF8_4, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD
};

/*
 *	static void *goutf8_continue[] =
 *	{
 *		[0 ... 127] = &&l_bad,
 *		[128 ... 191] = &&l_utf_continue,
 *		[192 ... 255] = &&l_bad
 *	};
 */
static int8_t goutf8_continue[256] = {
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE, A_UTF_CONTINUE,
	A_UTF_CONTINUE, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD
};

/*
 *	static void *goesc[] =
 *	{
 *		[0 ... 255] = &&l_bad,
 *		['"'] = &&l_unesc, ['\\'] = &&l_unesc,
 *		['/'] = &&l_unesc, ['b'] = &&l_unesc,
 *		['f'] = &&l_unesc, ['n'] = &&l_unesc,
 *		['r'] = &&l_unesc, ['t'] = &&l_unesc, ['u'] = &&l_unesc
 *	};
 */
static int8_t goesc[256] = {
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_UNESC,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_UNESC, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_UNESC, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_UNESC, A_BAD, A_BAD, A_BAD, A_UNESC, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_UNESC, A_BAD,
	A_BAD, A_BAD, A_UNESC, A_BAD, A_UNESC, A_UNESC, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD,
	A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD, A_BAD
};

/*
 * __process_value
 *	Deal with special config values like true / false.
 */
static void
__process_value(WT_CONFIG_ITEM *value)
{
    if (value->type == ITEM_ID && value->len > 0) {
	if (strncasecmp(value->str, "true", value->len) == 0) {
	    value->type = ITEM_NUM;
	    value->val = 1;
	} else if (strncasecmp(value->str, "false", value->len) == 0) {
	    value->type = ITEM_NUM;
	    value->val = 0;
	}
    } else if (value->type == ITEM_NUM) {
	    /* !!! TODO need to add support for "KB", "MB", etc. suffixes. */
	    value->val = strtol(value->str, NULL, 10);
    }
}

/*
 * __wt_config_next --
 *	Get the next config item in the string.
 */
int
__wt_config_next(WT_CONFIG *conf, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG_ITEM *out = key;
	int utf8_remain = 0;
	static WT_CONFIG_ITEM default_value = {
		"1", 1, 1, ITEM_NUM
	};

	key->len = 0;
	*value = default_value;

	if (conf->go == NULL)
		conf->go = gostruct;

	while (conf->cur < conf->end) {
		switch (conf->go[(int)*conf->cur]) {
		case A_LOOP:
			break;

		case A_BAD:
			return (EINVAL);

		case A_DOWN:
			--conf->depth;
			CAP(0);
			break;

		case A_UP:
			if (conf->top == -1)
				conf->top = 1;
			PUSH(0, ITEM_STRUCT);
			++conf->depth;
			break;

		case A_VALUE:
			if (conf->depth == conf->top) {
				if (out == value)
					return (EINVAL);
				out = value;
			}
			break;

		case A_NEXT:
			if (conf->depth == conf->top && key->len > 0) {
				++conf->cur;
				goto val;
			} else
				break;

		case A_QDOWN:
			CAP(-1);
			conf->go = gostruct;
			break;

		case A_QUP:
			PUSH(1, ITEM_STRING);
			conf->go = gostring;
			break;

		case A_ESC:
			conf->go = goesc;
			break;

		case A_UNESC:
			conf->go = gostring;
			break;

		case A_BARE:
			PUSH(0, ITEM_ID);
			conf->go = gobare;
			break;

		case A_NUMBARE:
			PUSH(0, ITEM_NUM);
			conf->go = gobare;
			break;

		case A_UNBARE:
			CAP(-1);
			conf->go = gostruct;
			continue;

		case A_UTF8_2:
			conf->go = goutf8_continue;
			utf8_remain = 1;
			break;

		case A_UTF8_3:
			conf->go = goutf8_continue;
			utf8_remain = 2;
			break;

		case A_UTF8_4:
			conf->go = goutf8_continue;
			utf8_remain = 3;
			break;

		case A_UTF_CONTINUE:
			if (!--utf8_remain)
				conf->go = gostring;
			break;
		}

		conf->cur++;
	}

	/* Might have a trailing key/value without a closing brace */
	if (conf->go == gobare) {
		CAP(0);
		conf->go = gostruct;
	}

	/* Did we find something? */
	if (conf->depth <= conf->top && key->len > 0) {
val:		__process_value(value);
		return (0);
	}

	/* We're either at the end of the string or we failed to parse. */
	return ((conf->depth == 0) ? WT_NOTFOUND : EINVAL);
}

/*
 * __wt_config_getraw --
 *	Given a config parser, find the final value for a given key.
 */
int
__wt_config_getraw(
    WT_CONFIG *cparser, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG_ITEM k, v;
	int ret;

	while ((ret = __wt_config_next(cparser, &k, &v)) == 0) {
		if ((k.type == ITEM_STRING || k.type == ITEM_ID) &&
		    key->len == k.len &&
		    strncasecmp(key->str, k.str, k.len) == 0) {
			*value = v;
			return (0);
		}
	}

	return (ret);
}

/*
 * __wt_config_get --
 *	Given a NULL-terminated list of configuration strings, find
 *	the final value for a given key.
 */
int
__wt_config_get(const char **cfg, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG cparser;
	int found, ret;

	for (found = 0; *cfg != NULL; cfg++) {
		WT_RET(__wt_config_init(&cparser, *cfg));
		if ((ret = __wt_config_getraw(&cparser, key, value)) == 0)
			found = 1;
		else if (ret != WT_NOTFOUND)
			return (ret);
	}

	return (found ? 0 : WT_NOTFOUND);
}

/*
 * __wt_config_gets --
 *	Given a NULL-terminated list of configuration strings, find the final
 *	value for a given string key.
 */
int
__wt_config_gets(const char **cfg, const char *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG_ITEM key_item;

	key_item.type = ITEM_STRING;
	key_item.str = key;
	key_item.len = strlen(key);

	return (__wt_config_get(cfg, &key_item, value));
}

/*
 * __wt_config_getone --
 *	Get the value for a given key from a single config string.
 */
 int
__wt_config_getone(const char *cfg, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	const char *cfgs[2];

	cfgs[0] = cfg;
	cfgs[1] = NULL;
	return (__wt_config_get(cfgs, key, value));
}

/*
 * __wt_config_getones --
 *	Get the value for a given string key from a single config string.
 */
 int
__wt_config_getones(const char *cfg, const char *key, WT_CONFIG_ITEM *value)
{
	const char *cfgs[2];

	cfgs[0] = cfg;
	cfgs[1] = NULL;
	return (__wt_config_gets(cfgs, key, value));
}

/*
 * __wt_config_checklist --
 *	Given a NULL-terminated list of default configuration strings,
 *	check that all keys in an application-supplied config string appear
 *	somewhere in the defaults.
 */
int
__wt_config_checklist(SESSION *session,
    const char **defaults, const char *config)
{
	WT_CONFIG cparser;
	WT_CONFIG_ITEM k, v;
	int ret;

	/* It is always okay to pass NULL. */
	if (config == NULL)
		return (0);

	WT_RET(__wt_config_init(&cparser, config));
	while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
		if (k.type != ITEM_STRING && k.type != ITEM_ID) {
			__wt_errx(session,
			    "Invalid configuration key found: '%s'\n", k.str);
			return (EINVAL);
		}
		/*
		 * TODO
		 * Need to handle configuration keys that only match a prefix
		 * such as "index_1=(),index_2=()".
		 */
		if ((ret = __wt_config_get(defaults, &k, &v)) != 0) {
			if (ret == WT_NOTFOUND) {
				__wt_errx(session,
				    "Unknown configuration key found: '%.*s'\n",
				    k.len, k.str);
				ret = EINVAL;
			}
			return (ret);
		}
	}

	if (ret == WT_NOTFOUND)
		ret = 0;

	return (ret);
}

/*
 * __wt_config_check --
 *	Given a default configuration string, check that all keys in an
 *	application-supplied config string appear somewhere in the defaults.
 */
int
__wt_config_check(SESSION *session, const char *defaults, const char *config)
{
	const char *defs[2];
	defs[0] = defaults;
	defs[1] = NULL;

	return (__wt_config_checklist(session, defs, config));
}
