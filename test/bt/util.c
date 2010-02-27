/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wts.h"

char *
fname(const char *prefix, const char *name)
{
	static char buf[128];

	(void)snprintf(buf, sizeof(buf), "__%s.%s", prefix, name);
	return (buf);
}

void
key_gen(DBT *key, u_int64_t key_cnt)
{
	static char buf[256];
	size_t len, tlen;

	/*
	 * The key is a variable length item with a leading 10-digit value.
	 * Since we have to be able re-construct it from the record number
	 * (when doing row lookups), we use a randomly generated length that
	 * we can reproduce.
	 *
	 * We store it in a static buffer, correct the size, just in case.
	 */
	if ((tlen = g.c_key_len) == 0)
		tlen = g.key_rand_len[key_cnt %
		    (sizeof(g.key_rand_len) / sizeof(g.key_rand_len))];
	if (tlen > sizeof(buf))
		tlen = sizeof(buf);

	/* The key always starts with a 10-digit string (the specified cnt). */
	len = snprintf(buf, sizeof(buf), "%010llu", key_cnt);
	if (len < tlen)
		memset(buf + len, 'a', tlen - len);
	key->data = buf;
	key->size = tlen;
}

void
data_gen(DBT *data)
{
	static char buf[5 * 1024];
	size_t i, len;

	/* Set buffer contents. */
	if (buf[0] == '\0')
		for (i = 0; i < sizeof(buf); ++i)
			buf[i] = 'A' + i % 26;

	/*
	 * The data is a variable length item.
	 * We store it in a static buffer, correct the size, just in case.
	 */
	if ((len = g.c_fixed_length) == 0 && (len = g.c_data_len) == 0)
		len = MMRAND(g.c_data_min, g.c_data_max);
	if (len > sizeof(buf))
		len = sizeof(buf);

	/*
	 * If we're repeat compression, use something different 20% of the
	 * time, otherwise we end up with a single chunk of repeated data.
	 */
	data->data = buf + (g.c_repeat_comp ? (rand() % 5 == 0 ? 1 : 0) : 0);
	data->size = len;
}

void
track(const char *s, u_int64_t i)
{
	static int lastlen = 0;
	int len;
	char *p, msg[128];

	if (!isatty(STDOUT_FILENO))
		return;

	if (s == NULL)
		len = 0;
	else if (i == 0)
		len = snprintf(msg, sizeof(msg), "%s", s);
	else
		len = snprintf(msg, sizeof(msg), "%s %llu", s, i);

	for (p = msg + len; len < lastlen; ++len)
		*p++ = ' ';
	lastlen = len;
	for (; len > 0; --len)
		*p++ = '\b';
	*p = '\0';
	(void)printf("%s", msg);
	(void)fflush(stdout);
}
