/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
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

#ifndef MAX
#define	MAX(a, b)	(((a) > (b)) ? (a) : (b))
#endif

/*
 * dmalloc --
 *	Call malloc, dying on failure.
 */
void *
dmalloc(size_t len)
{
	void *p;

	if ((p = malloc(len)) == NULL)
		die(errno, "malloc");
	return (p);
}

/*
 * dstrdup --
 *	Call strdup, dying on failure.
 */
char *
dstrdup(const char *str)
{
	char *p;

	if ((p = strdup(str)) == NULL)
		die(errno, "strdup");
	return (p);
}

static inline uint32_t
kv_len(WT_RAND_STATE *rnd, uint64_t keyno, uint32_t min, uint32_t max)
{
	/*
	 * Focus on relatively small key/value items, admitting the possibility
	 * of larger items.  Pick a size close to the minimum most of the time,
	 * only create a larger item 1 in 20 times, and a really big item 1 in
	 * 1000 times. (Configuration can force large key/value minimum sizes,
	 * where every key/value item is an overflow.)
	 */
	if (keyno % 1000 == 0 && max < KILOBYTE(80)) {
		min = KILOBYTE(80);
		max = KILOBYTE(100);
	} else if (keyno % 20 != 0 && max > min + 20)
		max = min + 20;
	return (mmrand(rnd, min, max));
}

void
key_len_setup(void)
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
		g.key_rand_len[i] =
		    kv_len(NULL, (uint64_t)i, g.c_key_min, g.c_key_max);
}

void
key_gen_setup(uint8_t **keyp)
{
	uint8_t *key;
	size_t i, len;

	*keyp = NULL;

	len = MAX(KILOBYTE(100), g.c_key_max);
	key = dmalloc(len);
	for (i = 0; i < len; ++i)
		key[i] = (uint8_t)("abcdefghijklmnopqrstuvwxyz"[i % 26]);
	*keyp = key;
}

static void
key_gen_common(uint8_t *key, size_t *sizep, uint64_t keyno, int suffix)
{
	int len;

	/*
	 * The key always starts with a 10-digit string (the specified cnt)
	 * followed by two digits, a random number between 1 and 15 if it's
	 * an insert, otherwise 00.
	 */
	len = sprintf((char *)key, "%010" PRIu64 ".%02d", keyno, suffix);

	/*
	 * In a column-store, the key is only used for BDB, and so it doesn't
	 * need a random length.
	 */
	if (g.type == ROW) {
		key[len] = '/';
		len = (int)g.key_rand_len[keyno %
		    (sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]))];
	}
	*sizep = (size_t)len;
}

void
key_gen(uint8_t *key, size_t *sizep, uint64_t keyno)
{
	key_gen_common(key, sizep, keyno, 0);
}

void
key_gen_insert(WT_RAND_STATE *rnd, uint8_t *key, size_t *sizep, uint64_t keyno)
{
	key_gen_common(key, sizep, keyno, (int)mmrand(rnd, 1, 15));
}

static uint32_t val_dup_data_len;	/* Length of duplicate data items */

void
val_gen_setup(WT_RAND_STATE *rnd, uint8_t **valp)
{
	uint8_t *val;
	size_t i, len;

	*valp = NULL;

	/*
	 * Set initial buffer contents to recognizable text.
	 *
	 * Add a few extra bytes in order to guarantee we can always offset
	 * into the buffer by a few extra bytes, used to generate different
	 * data for column-store run-length encoded files.
	 */
	len = MAX(KILOBYTE(100), g.c_value_max) + 20;
	val = dmalloc(len);
	for (i = 0; i < len; ++i)
		val[i] = (uint8_t)("ABCDEFGHIJKLMNOPQRSTUVWXYZ"[i % 26]);

	*valp = val;

	val_dup_data_len = kv_len(rnd,
	    (uint64_t)mmrand(rnd, 1, 20), g.c_value_min, g.c_value_max);
}

void
val_gen(WT_RAND_STATE *rnd, uint8_t *val, size_t *sizep, uint64_t keyno)
{
	/*
	 * Fixed-length records: take the low N bits from the last digit of
	 * the record number.
	 */
	if (g.type == FIX) {
		switch (g.c_bitcnt) {
		case 8: val[0] = (uint8_t)mmrand(rnd, 1, 0xff); break;
		case 7: val[0] = (uint8_t)mmrand(rnd, 1, 0x7f); break;
		case 6: val[0] = (uint8_t)mmrand(rnd, 1, 0x3f); break;
		case 5: val[0] = (uint8_t)mmrand(rnd, 1, 0x1f); break;
		case 4: val[0] = (uint8_t)mmrand(rnd, 1, 0x0f); break;
		case 3: val[0] = (uint8_t)mmrand(rnd, 1, 0x07); break;
		case 2: val[0] = (uint8_t)mmrand(rnd, 1, 0x03); break;
		case 1: val[0] = 1; break;
		}
		*sizep = 1;
		return;
	}

	/*
	 * WiredTiger doesn't store zero-length data items in row-store files,
	 * test that by inserting a zero-length data item every so often.
	 */
	if (keyno % 63 == 0) {
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
	    g.c_repeat_data_pct != 0 &&
	    mmrand(rnd, 1, 100) < g.c_repeat_data_pct) {
		(void)strcpy((char *)val, "DUPLICATEV");
		val[10] = '/';
		*sizep = val_dup_data_len;
	} else {
		(void)sprintf((char *)val, "%010" PRIu64, keyno);
		val[10] = '/';
		*sizep = kv_len(rnd, keyno, g.c_value_min, g.c_value_max);
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
		    "%4d: %s: "
		    "search %" PRIu64 "%s, "
		    "insert %" PRIu64 "%s, "
		    "update %" PRIu64 "%s, "
		    "remove %" PRIu64 "%s",
		    g.run_cnt, tag,
		    tinfo->search > M(9) ? tinfo->search / M(1) : tinfo->search,
		    tinfo->search > M(9) ? "M" : "",
		    tinfo->insert > M(9) ? tinfo->insert / M(1) : tinfo->insert,
		    tinfo->insert > M(9) ? "M" : "",
		    tinfo->update > M(9) ? tinfo->update / M(1) : tinfo->update,
		    tinfo->update > M(9) ? "M" : "",
		    tinfo->remove > M(9) ? tinfo->remove / M(1) : tinfo->remove,
		    tinfo->remove > M(9) ? "M" : "");

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
 * path_setup --
 *	Build the standard paths and shell commands we use.
 */
void
path_setup(const char *home)
{
	size_t len;

	/* Home directory. */
	g.home = dstrdup(home == NULL ? "RUNDIR" : home);

	/* Log file. */
	len = strlen(g.home) + strlen("log") + 2;
	g.home_log = dmalloc(len);
	snprintf(g.home_log, len, "%s/%s", g.home, "log");

	/* RNG log file. */
	len = strlen(g.home) + strlen("rand") + 2;
	g.home_rand = dmalloc(len);
	snprintf(g.home_rand, len, "%s/%s", g.home, "rand");

	/* Run file. */
	len = strlen(g.home) + strlen("CONFIG") + 2;
	g.home_config = dmalloc(len);
	snprintf(g.home_config, len, "%s/%s", g.home, "CONFIG");

	/* Statistics file. */
	len = strlen(g.home) + strlen("stats") + 2;
	g.home_stats = dmalloc(len);
	snprintf(g.home_stats, len, "%s/%s", g.home, "stats");

	/* Backup directory. */
	len = strlen(g.home) + strlen("BACKUP") + 2;
	g.home_backup = dmalloc(len);
	snprintf(g.home_backup, len, "%s/%s", g.home, "BACKUP");

	/* BDB directory. */
	len = strlen(g.home) + strlen("bdb") + 2;
	g.home_bdb = dmalloc(len);
	snprintf(g.home_bdb, len, "%s/%s", g.home, "bdb");

	/*
	 * Home directory initialize command: create the directory if it doesn't
	 * exist, else remove everything except the RNG log file, create the KVS
	 * subdirectory.
	 *
	 * Redirect the "cd" command to /dev/null so chatty cd implementations
	 * don't add the new working directory to our output.
	 */
#undef	CMD
#ifdef _WIN32
#define	CMD	"test -e %s || mkdir %s; "				\
		"cd %s && del /s /q * >:nul && rd /s /q KVS; "		\
		"mkdir KVS"
#else
#define	CMD	"test -e %s || mkdir %s; "				\
		"cd %s > /dev/null && rm -rf `ls | sed /rand/d`; "	\
		"mkdir KVS"
#endif
	len = strlen(g.home) * 3 + strlen(CMD) + 1;
	g.home_init = dmalloc(len);
	snprintf(g.home_init, len, CMD, g.home, g.home, g.home);

	/* Backup directory initialize command, remove and re-create it. */
#undef	CMD
#ifdef _WIN32
#define	CMD	"del /s /q >:nul && mkdir %s"
#else
#define	CMD	"rm -rf %s && mkdir %s"
#endif
	len = strlen(g.home_backup) * 2 + strlen(CMD) + 1;
	g.home_backup_init = dmalloc(len);
	snprintf(g.home_backup_init, len, CMD, g.home_backup, g.home_backup);

	/*
	 * Salvage command, save the interesting files so we can replay the
	 * salvage command as necessary.
	 *
	 * Redirect the "cd" command to /dev/null so chatty cd implementations
	 * don't add the new working directory to our output.
	 */
#undef	CMD
#ifdef _WIN32
#define	CMD								\
	"cd %s && "							\
	"rd /q /s slvg.copy & mkdir slvg.copy && "			\
	"copy WiredTiger* slvg.copy\\ >:nul && copy wt* slvg.copy\\ >:nul"
#else
#define	CMD								\
	"cd %s > /dev/null && "						\
	"rm -rf slvg.copy && mkdir slvg.copy && "			\
	"cp WiredTiger* wt* slvg.copy/"
#endif
	len = strlen(g.home) + strlen(CMD) + 1;
	g.home_salvage_copy = dmalloc(len);
	snprintf(g.home_salvage_copy, len, CMD, g.home);
}

/*
 * rng --
 *	Return a random number.
 */
uint32_t
rng(WT_RAND_STATE *rnd)
{
	char buf[64];
	uint32_t r;

	/*
	 * Threaded operations have their own RNG information, otherwise we
	 * use the default.
	 */
	if (rnd == NULL)
		rnd = &g.rnd;

	/*
	 * We can reproduce a single-threaded run based on the random numbers
	 * used in the initial run, plus the configuration files.
	 *
	 * Check g.replay and g.rand_log_stop: multithreaded runs log/replay
	 * until they get to the operations phase, then turn off log/replay,
	 * threaded operation order can't be replayed.
	 */
	if (g.rand_log_stop)
		return (__wt_random(rnd));

	if (g.replay) {
		if (fgets(buf, sizeof(buf), g.randfp) == NULL) {
			if (feof(g.randfp)) {
				fprintf(stderr,
				    "\n" "end of random number log reached\n");
				exit(EXIT_SUCCESS);
			}
			die(errno, "random number log");
		}

		return ((uint32_t)strtoul(buf, NULL, 10));
	}

	r = __wt_random(rnd);

	/* Save and flush the random number so we're up-to-date on error. */
	(void)fprintf(g.randfp, "%" PRIu32 "\n", r);
	(void)fflush(g.randfp);

	return (r);
}

/*
 * fclose_and_clear --
 *	Close a file and clear the handle so we don't close twice.
 */
void
fclose_and_clear(FILE **fpp)
{
	FILE *fp;

	if ((fp = *fpp) == NULL)
		return;
	*fpp = NULL;
	if (fclose(fp) != 0)
		die(errno, "fclose");
	return;
}
