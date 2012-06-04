/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __config_err --
 *	Error message and return for config string parse failures.
 */
static int
__config_err(WT_CONFIG *conf, const char *msg, int err)
{
	WT_RET_MSG(conf->session, err == 0 ? EINVAL : err,
	    "Error parsing '%.*s' at byte %u: %s",
	    (int)(conf->end - conf->orig), conf->orig,
	    (u_int)(conf->cur - conf->orig), msg);
}

/*
 * __wt_config_initn --
 *	Initialize a config handle, used to iterate through a config string of
 *	specified length.
 */
int
__wt_config_initn(
    WT_SESSION_IMPL *session, WT_CONFIG *conf, const char *str, size_t len)
{
	conf->session = session;
	conf->orig = conf->cur = str;
	conf->end = str + len;
	conf->depth = 0;
	conf->top = -1;
	conf->go = NULL;

	return (0);
}

/*
 * __wt_config_init --
 *	Initialize a config handle, used to iterate through a NUL-terminated
 *	config string.
 */
int
__wt_config_init(WT_SESSION_IMPL *session, WT_CONFIG *conf, const char *str)
{
	size_t len;

	len = (str == NULL) ? 0 : strlen(str);

	return (__wt_config_initn(session, conf, str, len));
}

/*
 * __wt_config_subinit --
 *	Initialize a config handle, used to iterate through a config string
 *	extracted from another config string (used for parsing nested
 *	structures).
 */
int
__wt_config_subinit(
    WT_SESSION_IMPL *session, WT_CONFIG *conf, WT_CONFIG_ITEM *item)
{
	return (__wt_config_initn(session, conf, item->str, item->len));
}

#define	PUSH(i, t) do {							\
	if (conf->top == -1)						\
		conf->top = conf->depth;				\
	if (conf->depth == conf->top) {					\
		if (out->len > 0)					\
			return __config_err(conf,			\
			    "New value starts without a separator", 0);	\
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
static int
__process_value(WT_CONFIG *conf, WT_CONFIG_ITEM *value)
{
	char *endptr;

	/* Empty values are okay: we can't do anything interesting with them. */
	if (value->len == 0)
		return (0);

	if (value->type == ITEM_ID) {
		if (strncasecmp(value->str, "true", value->len) == 0) {
			value->type = ITEM_NUM;
			value->val = 1;
		} else if (strncasecmp(value->str, "false", value->len) == 0) {
			value->type = ITEM_NUM;
			value->val = 0;
		}
	} else if (value->type == ITEM_NUM) {
		errno = 0;
		value->val = strtoll(value->str, &endptr, 10);

		/* Check any leftover characters. */
		while (endptr < value->str + value->len)
			switch (*endptr++) {
			case 'b':
			case 'B':
				/* Byte: no change. */
				break;
			case 'k':
			case 'K':
				value->val <<= 10;
				break;
			case 'm':
			case 'M':
				value->val <<= 20;
				break;
			case 'g':
			case 'G':
				value->val <<= 30;
				break;
			case 't':
			case 'T':
				value->val <<= 40;
				break;
			case 'p':
			case 'P':
				value->val <<= 50;
				break;
			default:
				/*
				 * We didn't get a well-formed number.  That
				 * might be okay, the required type will be
				 * checked by __wt_config_check.
				 */
				value->type = ITEM_ID;
				break;
			}

		/*
		 * If we parsed the the whole string but the number is out of
		 * range, report an error.  Don't report an error for strings
		 * that aren't well-formed integers: if an integer is expected,
		 * that will be caught by __wt_config_check.
		 */
		if (value->type == ITEM_NUM && errno == ERANGE)
			return (
			    __config_err(conf, "Number out of range", ERANGE));
	}

	return (0);
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
		"", 0, 1, ITEM_NUM
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
			return __config_err(conf, "Unexpected character", 0);

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
				/*
				 * Special case: ':' is permitted in unquoted
				 * values.
				 */
				if (out == value && *conf->cur != ':')
					return __config_err(conf,
					    "Value already complete", 0);
				out = value;
			}
			break;

		case A_NEXT:
			/*
			 * If we're at the top level and we have a complete
			 * key (and optional value), we're done.
			 */
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
		CAP(-1);
		conf->go = gostruct;
	}

	/* Did we find something? */
	if (conf->depth <= conf->top && key->len > 0)
val:		return (__process_value(conf, value));

	/* We're either at the end of the string or we failed to parse. */
	if (conf->depth == 0)
		return (WT_NOTFOUND);

	return __config_err(conf,
	    "Closing brackets missing from config string", 0);
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
	WT_DECL_RET;
	int found;

	found = 0;
	while ((ret = __wt_config_next(cparser, &k, &v)) == 0) {
		if ((k.type == ITEM_STRING || k.type == ITEM_ID) &&
		    key->len == k.len &&
		    strncasecmp(key->str, k.str, k.len) == 0) {
			*value = v;
			found = 1;
		}
	}

	return (found && ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __wt_config_get --
 *	Given a NULL-terminated list of configuration strings, find
 *	the final value for a given key.
 */
int
__wt_config_get(WT_SESSION_IMPL *session,
    const char **cfg, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG cparser;
	WT_DECL_RET;
	int found;

	for (found = 0; *cfg != NULL; cfg++) {
		WT_RET(__wt_config_init(session, &cparser, *cfg));
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
__wt_config_gets(WT_SESSION_IMPL *session,
    const char **cfg, const char *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG_ITEM key_item;

	key_item.type = ITEM_STRING;
	key_item.str = key;
	key_item.len = strlen(key);

	return (__wt_config_get(session, cfg, &key_item, value));
}

/*
 * __wt_config_getone --
 *	Get the value for a given key from a single config string.
 */
 int
__wt_config_getone(WT_SESSION_IMPL *session,
    const char *cfg, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	const char *cfgs[] = { cfg, NULL };
	return (__wt_config_get(session, cfgs, key, value));
}

/*
 * __wt_config_getones --
 *	Get the value for a given string key from a single config string.
 */
 int
__wt_config_getones(WT_SESSION_IMPL *session,
    const char *cfg, const char *key, WT_CONFIG_ITEM *value)
{
	const char *cfgs[] = { cfg, NULL };
	return (__wt_config_gets(session, cfgs, key, value));
}

/*
 * __wt_config_subgetraw --
 *	Get the value for a given key from a config string in a WT_CONFIG_ITEM.
 *	This is useful for dealing with nested structs in config strings.
 */
 int
__wt_config_subgetraw(WT_SESSION_IMPL *session,
    WT_CONFIG_ITEM *cfg, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG cparser;

	WT_RET(__wt_config_initn(session, &cparser, cfg->str, cfg->len));
	return (__wt_config_getraw(&cparser, key, value));
}

/*
 * __wt_config_subgets --
 *	Get the value for a given key from a config string in a WT_CONFIG_ITEM.
 *	This is useful for dealing with nested structs in config strings.
 */
 int
__wt_config_subgets(WT_SESSION_IMPL *session,
    WT_CONFIG_ITEM *cfg, const char *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG_ITEM key_item;

	key_item.str = key;
	key_item.len = strlen(key);

	return (__wt_config_subgetraw(session, cfg, &key_item, value));
}
