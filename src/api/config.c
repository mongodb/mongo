/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

int
config_init(WT_CONFIG *conf, const char *confstr, int len)
{
	conf->orig = conf->cur = confstr;
	conf->end = confstr + len;
	conf->depth = 0;
	conf->top = -1;
	conf->go = NULL;

	return (0);
}

#define	PUSH(i, t) do {							\
	if (conf->top == -1)						\
		conf->top = conf->depth;				\
	if (conf->depth == conf->top) {					\
		out->type = t;						\
		out->str = (conf->cur + i);				\
	}								\
} while (0)

#define	CAP(i) do {							\
	if (conf->depth == conf->top)					\
		out->len = (size_t)((conf->cur + i + 1) - out->str);	\
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

int
config_next(WT_CONFIG *conf, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG_ITEM *out = key;
	int utf8_remain = 0;
	static WT_CONFIG_ITEM default_value = {
		"1", 1, 1, ITEM_NUM
	};

	key->len = 0;

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
				/* Handle the case with no value */
				if (out == key)
					*value = default_value;
				++conf->cur;
				return (0);
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
		if (out == key)
			*value = default_value;
		return (0);
	}

	/* We're either at the end of the string or we failed to parse. */
	return ((conf->depth == 0) ? WT_NOTFOUND : EINVAL);
}
