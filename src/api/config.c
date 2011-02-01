/* Copyright (c) 2011 WiredTiger, Inc.  All rights reserved. */

// Some code from js0n by jeremie miller - 2010
// public domain, https://github.com/quartzjer/js0n
//
// XXX This code relies on two GCC extensions.
//
// The first allows labels to be treated as values.  This is handy for
// state machines but will require a rewrite with a big switch statement
// for portability.
//
// The second is for range array initialization.
// The syntax for subscription a single element is C99:
// <quote>
// To specify an array index, write [index] = before the element value.
// For example,
//
// int a[6] = { [4] = 29, [2] = 15 };
// ...
// To initialize a range of elements to the same value, write
// [first ... last] = value. This is a GNU extension. For example,
//
// int widths[] = { [0 ... 9] = 1, [10 ... 99] = 2, [100] = 3 };
// </quote>
// 
// Both are relatively easy to transform into portable C99, but a pain until // the interface is stable.

#include <wt_int.h>

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

#define PUSH(i, t) do {							\
	if(conf->top == -1)						\
		conf->top = conf->depth;				\
	if(conf->depth == conf->top) {					\
		out->type = t;						\
		out->str = (conf->cur + i);				\
	}								\
} while (0)

#define CAP(i) do {							\
	if(conf->depth == conf->top)					\
		out->len = (conf->cur + i + 1) - out->str;		\
} while (0)

int
config_next(WT_CONFIG *conf, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
	WT_CONFIG_ITEM *out = key;
	int utf8_remain = 0;
	static void *gostruct[] = 
	{
		[0 ... 255] = &&l_bad,
		['\t'] = &&l_loop, [' '] = &&l_loop,
		['\r'] = &&l_loop, ['\n'] = &&l_loop,
		['"'] = &&l_qup,
		[':'] = &&l_value, ['='] = &&l_value,
		[','] = &&l_next,
		// tracking [] and {} individually would allow fuller
		// validation but is really messy
		['('] = &&l_up, [')'] = &&l_down,
		['['] = &&l_up, [']'] = &&l_down,
		['{'] = &&l_up, ['}'] = &&l_down,
		// bare identifiers
		['-'] = &&l_numbare,
		['0' ... '9'] = &&l_numbare,
		['_'] = &&l_bare,
		['A' ... 'Z'] = &&l_bare, ['a' ... 'z'] = &&l_bare,
	};
	static void *gobare[] = 
	{
		[0 ... 31] = &&l_bad,
		// could be more pedantic/validation-checking
		[32 ... 126] = &&l_loop,
		['\t'] = &&l_unbare, [' '] = &&l_unbare,
		['\r'] = &&l_unbare, ['\n'] = &&l_unbare,
		[':'] = &&l_unbare, ['='] = &&l_unbare,
		[','] = &&l_unbare,
		[')'] = &&l_unbare, [']'] = &&l_unbare, ['}'] = &&l_unbare,
		[127 ... 255] = &&l_bad
	};
	static void *gostring[] = 
	{
		[0 ... 31] = &&l_bad, [127] = &&l_bad,
		[32 ... 126] = &&l_loop,
		['\\'] = &&l_esc, ['"'] = &&l_qdown,
		[128 ... 191] = &&l_bad,
		[192 ... 223] = &&l_utf8_2,
		[224 ... 239] = &&l_utf8_3,
		[240 ... 247] = &&l_utf8_4,
		[248 ... 255] = &&l_bad
	};
	static void *goutf8_continue[] =
	{
		[0 ... 127] = &&l_bad,
		[128 ... 191] = &&l_utf_continue,
		[192 ... 255] = &&l_bad
	};
	static void *goesc[] = 
	{
		[0 ... 255] = &&l_bad,
		['"'] = &&l_unesc, ['\\'] = &&l_unesc,
		['/'] = &&l_unesc, ['b'] = &&l_unesc,
		['f'] = &&l_unesc, ['n'] = &&l_unesc,
		['r'] = &&l_unesc, ['t'] = &&l_unesc, ['u'] = &&l_unesc
	};
	static WT_CONFIG_ITEM default_value = {
		"1", 1, 1, ITEM_NUM
	};

	key->len = 0;
	
	if (conf->go == NULL)
		conf->go = gostruct;

	for(; conf->cur < conf->end; conf->cur++)
	{
		goto *conf->go[*conf->cur];
l_loop:		;
	}

	// Might have a trailing key/value without a closing brace
	if (conf->go == gobare) {
		CAP(0);
		conf->go = gostruct;
	}

	if (conf->depth <= conf->top && key->len > 0) {
		if (out == key)
			*value = default_value;
		return (0);
	}

	// 0 if successful full parse, >0 for incomplete data
	return ((conf->depth == 0) ? WT_NOTFOUND : EINVAL);

l_bad:
	return (EINVAL);

l_down:
	--conf->depth;
	CAP(0);
	goto l_loop;

l_up:
	if(conf->top == -1)
		conf->top = 1;
	PUSH(0, ITEM_STRUCT);
	++conf->depth;
	goto l_loop;

l_value:
	if (conf->depth == conf->top) {
		if (out == value)
			goto l_bad;
		out = value;
	}
	goto l_loop;

l_next:
	if (conf->depth == conf->top && key->len > 0) {
		// Handle the case with no value
		if (out == key)
			*value = default_value;
		++conf->cur;
		return (0);
	} else
		goto l_loop;

l_qdown:
	CAP(-1);
	conf->go = gostruct;
	goto l_loop;

l_qup:
	PUSH(1, ITEM_STRING);
	conf->go = gostring;
	goto l_loop;

l_esc:
	conf->go = goesc;
	goto l_loop;

l_unesc:
	conf->go = gostring;
	goto l_loop;

l_bare:
	PUSH(0, ITEM_ID);
	conf->go = gobare;
	goto l_loop;

l_numbare:
	PUSH(0, ITEM_NUM);
	conf->go = gobare;
	goto l_loop;

l_unbare:
	CAP(-1);
	conf->go = gostruct;
	goto *conf->go[*conf->cur];

l_utf8_2:
	conf->go = goutf8_continue;
	utf8_remain = 1;
	goto l_loop;

l_utf8_3:
	conf->go = goutf8_continue;
	utf8_remain = 2;
	goto l_loop;

l_utf8_4:
	conf->go = goutf8_continue;
	utf8_remain = 3;
	goto l_loop;

l_utf_continue:
	if (!--utf8_remain)
		conf->go = gostring;
	goto l_loop;
}
