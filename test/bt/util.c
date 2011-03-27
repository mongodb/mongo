/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wts.h"

char *
fname(const char *name)
{
	static char buf[128];

	(void)snprintf(buf, sizeof(buf), "__%s.%s", WT_PREFIX, name);
	return (buf);
}

void
key_gen(void *datap, uint32_t *sizep, u_int64_t keyno, int insert)
{
	int len;

	/*
	 * The key always starts with a 10-digit string (the specified cnt).
	 * If the operation is an insert, append a unique trailing string.
	 */
	len = insert ?
	    sprintf(g.key_gen_buf, "%010llu.%d", keyno, (int)MMRAND(0, 10)) :
	    sprintf(g.key_gen_buf, "%010llu", keyno);
	g.key_gen_buf[len] = '/';

	*(void **)datap = g.key_gen_buf;
	*sizep = g.key_rand_len[keyno %
	    (sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]))];
}

void
key_gen_setup(void)
{
	size_t i;

	/*
	 * The key is a variable length item with a leading 10-digit value.
	 * Since we have to be able re-construct it from the record number
	 * (when doing row lookups), we pre-load a set of random lengths in
	 * a lookup table, and then use the record number to choose one of
	 * the pre-loaded lengths.
	 *
	 * Fill in the random key lengths.
	 */
	if (g.key_gen_buf != NULL) {
		free(g.key_gen_buf);
		g.key_gen_buf = NULL;
	}
	for (i = 0; i < sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]); ++i)
		g.key_rand_len[i] = MMRAND(g.c_key_min, g.c_key_max);
		
	if ((g.key_gen_buf = malloc(g.c_key_max)) == NULL) {
		fprintf(stderr, "%s: %s\n", g.progname, strerror(errno));
		exit(EXIT_FAILURE);
	}
	for (i = 0; i < g.c_key_max; ++i)
		g.key_gen_buf[i] = 'a' + i % 26;
}

void
data_gen(void *datap, uint32_t *sizep, int grow_ok)
{
	static size_t blen;
	static u_int r;
	static char *buf;
	size_t i, len;
	char *p;

	/*
	 * Set initial buffer contents to reconizable text.
	 *
	 * Add a few extra bytes in order to guarantee we can always offset
	 * into the buffer by a few extra bytes, used to generate different
	 * data for column-store run-length encoded files.
	 */
	if (blen < g.c_data_max + 10) {
		if (buf != NULL) {
			free(buf);
			buf = NULL;
		}
		blen = g.c_data_max + 10;
		if ((buf = malloc(blen)) == NULL) {
			fprintf(stderr,
			    "%s: %s\n", g.progname, strerror(errno));
			exit(EXIT_FAILURE);
		}
		for (i = 0; i < blen; ++i)
			buf[i] = 'A' + i % 26;
	}

	/*
	 * The data always starts with a 10-digit number.  Change the leading
	 * number to ensure every data item is unique.
	 */
	sprintf(buf, "%010u", ++r);
	buf[10] = '/';

	p = buf;
	len = 0;
	switch (g.c_file_type) {
	case FIX:
		/*
		 * If doing run-length encoding on the data, use different data
		 * some percentage of the time, otherwise we'd end up with a
		 * single chunk of repeated data.   To do that, we just jump
		 * forward in the buffer by a small, random number of bytes.
		 */
		if (g.c_repeat_comp_pct != 0 &&
		    (u_int)wts_rand() % 100 > g.c_repeat_comp_pct)
			p += wts_rand() % 7;
		len = g.c_data_min;
		break;
	case VAR:
	case ROW:
		/*
		 * WiredTiger doesn't store zero-length records, and I want to
		 * test that: records divisible by 63 are always zero-length.
		 */
		if (r % 63 == 0)
			len = 0;
		else
			len = grow_ok ?
			    MMRAND(g.c_data_min, g.c_data_max) : g.c_data_max;
		break;
	}

	*(void **)datap = p;
	*sizep = len;
}

void
track(const char *s, u_int64_t i)
{
	static int lastlen = 0;
	int len, tlen;
	char *p, msg[128];

	if (!g.track)
		return;

	if (s == NULL)
		len = 0;
	else if (i == 0)
		len = snprintf(msg, sizeof(msg), "%4d: %s", g.run_cnt, s);
	else
		len = snprintf(msg, sizeof(msg),
		    "%4d: %s %llu", g.run_cnt, s, (unsigned long long)i);
	tlen = len;

	for (p = msg + len; tlen < lastlen; ++tlen)
		*p++ = ' ';
	*p = '\0';
	lastlen = len;

	(void)printf("%s\r", msg);
	(void)fflush(stdout);
}

/*
 * wts_rand --
 *	Return a random number.
 */
int
wts_rand(void)
{
	const char *p;
	char buf[64];
	u_int r;

	/*
	 * We can entirely reproduce a run based on the random numbers used
	 * in the initial run, plus the configuration files.  It would be
	 * nice to just log the initial RNG seed, rather than logging every
	 * random number generated, but we can't -- Berkeley DB calls rand()
	 * internally, and so that messes up the pattern of random numbers
	 * (and WT might call rand() in the future, who knows?)
	 */
	if (g.rand_log == NULL) {
		p = "__rand";
		if ((g.rand_log = fopen(p, g.replay ? "r" : "w")) == NULL) {
			fprintf(stderr, "%s: %s\n", p, strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (!g.replay)
			setvbuf(g.rand_log, NULL, _IOLBF, 0);
	}
	if (g.replay) {
		if (fgets(buf, sizeof(buf), g.rand_log) == NULL) {
			if (feof(g.rand_log)) {
				fprintf(stderr,
				    "end of random number log reached, "
				    "exiting\n");
			} else
				fprintf(stderr,
				    "random number log: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}

		r = (u_int)strtoul(buf, NULL, 10);
	} else {
		r = (u_int)rand();
		fprintf(g.rand_log, "%u\n", r);
	}
	return ((int)r);
}
