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
	static int first = 1;
	static char *buf;
	size_t i, len, klen;

	/*
	 * The key is a variable length item with a leading 10-digit value.
	 * Since we have to be able re-construct it from the record number
	 * (when doing row lookups), we pre-load a set of random lengths in
	 * a lookup table, and then use the record number to choose one of
	 * the pre-loaded lengths.
	 *
	 * Fill in the random key lengths.
	 */
	if (first) {
		first = 0;
		for (i = 0;
		    i < sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]); ++i)
			g.key_rand_len[i] = MMRAND(g.c_key_min, g.c_key_max);
		if ((buf = malloc(g.c_key_max)) == NULL) {
			fprintf(stderr,
			    "%s: %s\n", g.progname, strerror(errno));
			exit (EXIT_FAILURE);
		}
	}

	klen = g.key_rand_len[key_cnt %
	    (sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]))];

	/* The key always starts with a 10-digit string (the specified cnt). */
	len = snprintf(buf, g.c_key_max, "%010llu", key_cnt);
	if (len < klen)
		memset(buf + len, 'a', klen - len);
	key->data = buf;
	key->size = klen;
}

void
data_gen(DBT *data)
{
	static int first = 1;
	static char *buf;
	size_t i, len;
	char *p;

	/*
	 * Set buffer contents.
	 *
	 * If doing repeat compression, use different data some percentage of
	 * the time, otherwise we end up with a single chunk of repeated data.
	 * Add a few extra bytes in order to guarantee we can always offset
	 * into the buffer by a few bytes.
	 */
	if (first) {
		first = 0;
		len = g.c_data_max + 10;
		if ((buf = malloc(len)) == NULL) {
			fprintf(stderr,
			    "%s: %s\n", g.progname, strerror(errno));
			exit (EXIT_FAILURE);
		}
		for (i = 0; i < len; ++i)
			buf[i] = 'A' + i % 26;
	}

	switch (g.c_database_type) {
	case FIX:
		p = buf;
		if (g.c_repeat_comp != 0 ||
		    (u_int)rand() % 100 <= g.c_repeat_comp_pct)
			p += rand() % 7;
		len = g.c_data_min;
		break;
	case VAR:
	case ROW:
		p = buf;
		len = MMRAND(g.c_data_min, g.c_data_max);
		break;
	}

	data->data = p;
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
