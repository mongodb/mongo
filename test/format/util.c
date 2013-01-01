/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"

void
key_len_setup()
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
	for (i = 0; i < sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]); ++i)
		g.key_rand_len[i] = (uint16_t)MMRAND(g.c_key_min, g.c_key_max);
}

void
key_gen_setup(uint8_t **keyp)
{
	uint8_t *key;
	size_t i;

	if ((key = malloc(g.c_key_max)) == NULL)
		syserr("malloc");
	for (i = 0; i < g.c_key_max; ++i)
		key[i] = (uint8_t)("abcdefghijklmnopqrstuvwxyz"[i % 26]);
	*keyp = key;
}

void
key_gen(uint8_t *key, uint32_t *sizep, uint64_t keyno, int insert)
{
	int len, suffix;

	/*
	 * The key always starts with a 10-digit string (the specified cnt)
	 * followed by two digits, a random number between 1 and 15 if it's
	 * an insert, otherwise 00.
	 */
	suffix = insert ? (int)MMRAND(1, 15) : 0;
	len = sprintf((char *)key, "%010" PRIu64 ".%02d", keyno, suffix);

	/*
	 * In a column-store, the key is only used for BDB, and so it doesn't
	 * need a random length.
	 */
	if (g.type == ROW) {
		key[len] = '/';
		len = g.key_rand_len[keyno %
		    (sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]))];
	}
	*sizep = (uint32_t)len;
}

static uint32_t val_dup_data_len;	/* Length of duplicate data items */

void
val_gen_setup(uint8_t **valp)
{
	uint8_t *val;
	size_t i, len;

	/*
	 * Set initial buffer contents to recognizable text.
	 *
	 * Add a few extra bytes in order to guarantee we can always offset
	 * into the buffer by a few extra bytes, used to generate different
	 * data for column-store run-length encoded files.
	 */
	len = g.c_value_max + 20;
	if ((val = malloc(len)) == NULL)
		syserr("malloc");
	for (i = 0; i < len; ++i)
		val[i] = (uint8_t)("ABCDEFGHIJKLMNOPQRSTUVWXYZ"[i % 26]);

	*valp = val;

	val_dup_data_len = MMRAND(g.c_value_min, g.c_value_max);
}

void
value_gen(uint8_t *val, uint32_t *sizep, uint64_t keyno)
{
	/*
	 * Fixed-length records: take the low N bits from the last digit of
	 * the record number.
	 */
	if (g.type == FIX) {
		switch (g.c_bitcnt) {
		case 8: val[0] = MMRAND(1, 0xff); break;
		case 7: val[0] = MMRAND(1, 0x7f); break;
		case 6: val[0] = MMRAND(1, 0x3f); break;
		case 5: val[0] = MMRAND(1, 0x1f); break;
		case 4: val[0] = MMRAND(1, 0x0f); break;
		case 3: val[0] = MMRAND(1, 0x07); break;
		case 2: val[0] = MMRAND(1, 0x03); break;
		case 1: val[0] = 1; break;
		}
		*sizep = 1;
		return;
	}

	/*
	 * WiredTiger doesn't store zero-length data items in row-store files,
	 * test that by inserting a zero-length data item every so often.
	 * LSM doesn't support zero length items.
	 */
	if (keyno % 63 == 0 && strcmp("lsm", g.c_data_source) != 0) {
		val[0] = '\0';
		*sizep = 0;
		return;
	}

	/*
	 * Start the data with a 10-digit number.
	 *
	 * For row and non-repeated variable-length column-stores, change the
	 * leading number to ensure every data item is unique.  For repeated
	 * variable-length column-stores (that is, to test run-length encoding),
	 * use the same data value all the time.
	 */
	if ((g.type == ROW || g.type == VAR) &&
	    g.c_repeat_data_pct != 0 && MMRAND(1, 100) > g.c_repeat_data_pct) {
		(void)strcpy((char *)val, "DUPLICATEV");
		val[10] = '/';
		*sizep = val_dup_data_len;
	} else {
		(void)sprintf((char *)val, "%010" PRIu64, keyno);
		val[10] = '/';
		*sizep = MMRAND(g.c_value_min, g.c_value_max);
	}
}

void
track(const char *tag, uint64_t cnt, TINFO *tinfo)
{
	static int lastlen = 0;
	int len;
	char msg[128];

	if (!g.track || tag == NULL)
		return;

	if (tinfo == NULL && cnt == 0)
		len = snprintf(msg, sizeof(msg), "%4d: %s", g.run_cnt, tag);
	else if (tinfo == NULL)
		len = snprintf(
		    msg, sizeof(msg), "%4d: %s: %" PRIu64, g.run_cnt, tag, cnt);
	else
		len = snprintf(msg, sizeof(msg),
		    "%4d: %s: " "search %" PRIu64
		    ", insert %" PRIu64 ", update %" PRIu64 ", remove %" PRIu64,
		    g.run_cnt, tag,
		    tinfo->search, tinfo->insert, tinfo->update, tinfo->remove);

	if (lastlen > len) {
		memset(msg + len, ' ', (size_t)(lastlen - len));
		msg[lastlen] = '\0';
	}
	lastlen = len;

	if (printf("%s\r", msg) < 0)
		die(EIO, "printf");
	if (fflush(stdout) == EOF)
		die(errno, "fflush");
}

/*
 * wts_rand --
 *	Return a random number.
 */
uint32_t
wts_rand(void)
{
	char buf[64];
	uint32_t r;

	/* If we're threaded, it's not repeatable, ignore the log. */
	if (!SINGLETHREADED)
		return ((uint32_t)rand());

	/*
	 * We can entirely reproduce a run based on the random numbers used
	 * in the initial run, plus the configuration files.  It would be
	 * nice to just log the initial RNG seed, rather than logging every
	 * random number generated, but we can't -- Berkeley DB calls rand()
	 * internally, and so that messes up the pattern of random numbers
	 * (and WT might call rand() in the future, who knows?)
	 */
	if (g.replay) {
		if (g.rand_log == NULL &&
		   (g.rand_log = fopen("RUNDIR/rand", "r")) == NULL)
			die(errno, "fopen: RUNDIR/rand");

		if (fgets(buf, sizeof(buf), g.rand_log) == NULL) {
			if (feof(g.rand_log)) {
				fprintf(stderr,
				    "end of random number log reached, "
				    "exiting\n");
				exit(EXIT_SUCCESS);
			}
			die(errno, "feof: random number log");
		}

		r = (uint32_t)strtoul(buf, NULL, 10);
	} else {
		if (g.rand_log == NULL) {
			if ((g.rand_log = fopen("RUNDIR/rand", "w")) == NULL)
				die(errno, "fopen: RUNDIR/rand");
			(void)setvbuf(g.rand_log, NULL, _IOLBF, 0);

			/*
			 * Seed the random number generator for each new run (we
			 * know it's a new run when we re-open the log file).
			 */
			srand((u_int)(0xdeadbeef ^ (u_int)time(NULL)));
		}
		r = (uint32_t)rand();
		fprintf(g.rand_log, "%" PRIu32 "\n", r);
	}
	return (r);
}
