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

void
key_len_setup(void)
{
	size_t i;
	uint32_t max;

	/*
	 * The key is a variable length item with a leading 10-digit value.
	 * Since we have to be able re-construct it from the record number
	 * (when doing row lookups), we pre-load a set of random lengths in
	 * a lookup table, and then use the record number to choose one of
	 * the pre-loaded lengths.
	 *
	 * Fill in the random key lengths.
	 *
	 * Focus on relatively small items, admitting the possibility of larger
	 * items. Pick a size close to the minimum most of the time, only create
	 * a larger item 1 in 20 times.
	 */
	for (i = 0;
	    i < sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]); ++i) {
		max = g.c_key_max;
		if (i % 20 != 0 && max > g.c_key_min + 20)
			max = g.c_key_min + 20;
		g.key_rand_len[i] = mmrand(NULL, g.c_key_min, max);
	}
}

void
key_gen_setup(WT_ITEM *key)
{
	size_t i, len;
	char *p;

	len = MAX(KILOBYTE(100), g.c_key_max);
	p = dmalloc(len);
	for (i = 0; i < len; ++i)
		p[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];

	key->mem = p;
	key->memsize = len;
	key->data = key->mem;
	key->size = 0;
}

static void
key_gen_common(WT_ITEM *key, uint64_t keyno, int suffix)
{
	int len;
	char *p;

	p = key->mem;

	/*
	 * The key always starts with a 10-digit string (the specified cnt)
	 * followed by two digits, a random number between 1 and 15 if it's
	 * an insert, otherwise 00.
	 */
	len = sprintf(p, "%010" PRIu64 ".%02d", keyno, suffix);

	/*
	 * In a column-store, the key is only used for Berkeley DB inserts,
	 * and so it doesn't need a random length.
	 */
	if (g.type == ROW) {
		p[len] = '/';

		/*
		 * Because we're doing table lookup for key sizes, we weren't
		 * able to set really big keys sizes in the table, the table
		 * isn't big enough to keep our hash from selecting too many
		 * big keys and blowing out the cache. Handle that here, use a
		 * really big key 1 in 2500 times.
		 */
		len = keyno % 2500 == 0 && g.c_key_max < KILOBYTE(80) ?
		    KILOBYTE(80) :
		    (int)g.key_rand_len[keyno % WT_ELEMENTS(g.key_rand_len)];
	}

	key->data = key->mem;
	key->size = (size_t)len;
}

void
key_gen(WT_ITEM *key, uint64_t keyno)
{
	key_gen_common(key, keyno, 0);
}

void
key_gen_insert(WT_RAND_STATE *rnd, WT_ITEM *key, uint64_t keyno)
{
	key_gen_common(key, keyno, (int)mmrand(rnd, 1, 15));
}

static uint32_t val_dup_data_len;	/* Length of duplicate data items */

static inline uint32_t
value_len(WT_RAND_STATE *rnd, uint64_t keyno, uint32_t min, uint32_t max)
{
	/*
	 * Focus on relatively small items, admitting the possibility of larger
	 * items. Pick a size close to the minimum most of the time, only create
	 * a larger item 1 in 20 times, and a really big item 1 in somewhere
	 * around 2500 items.
	 */
	if (keyno % 2500 == 0 && max < KILOBYTE(80)) {
		min = KILOBYTE(80);
		max = KILOBYTE(100);
	} else if (keyno % 20 != 0 && max > min + 20)
		max = min + 20;
	return (mmrand(rnd, min, max));
}

void
val_gen_setup(WT_RAND_STATE *rnd, WT_ITEM *value)
{
	size_t i, len;
	char *p;

	memset(value, 0, sizeof(WT_ITEM));

	/*
	 * Set initial buffer contents to recognizable text.
	 *
	 * Add a few extra bytes in order to guarantee we can always offset
	 * into the buffer by a few extra bytes, used to generate different
	 * data for column-store run-length encoded files.
	 */
	len = MAX(KILOBYTE(100), g.c_value_max) + 20;
	p = dmalloc(len);
	for (i = 0; i < len; ++i)
		p[i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[i % 26];

	value->mem = p;
	value->memsize = len;
	value->data = value->mem;
	value->size = 0;

	val_dup_data_len = value_len(rnd,
	    (uint64_t)mmrand(rnd, 1, 20), g.c_value_min, g.c_value_max);
}

void
val_gen(WT_RAND_STATE *rnd, WT_ITEM *value, uint64_t keyno)
{
	char *p;

	p = value->mem;
	value->data = value->mem;

	/*
	 * Fixed-length records: take the low N bits from the last digit of
	 * the record number.
	 */
	if (g.type == FIX) {
		switch (g.c_bitcnt) {
		case 8: p[0] = (char)mmrand(rnd, 1, 0xff); break;
		case 7: p[0] = (char)mmrand(rnd, 1, 0x7f); break;
		case 6: p[0] = (char)mmrand(rnd, 1, 0x3f); break;
		case 5: p[0] = (char)mmrand(rnd, 1, 0x1f); break;
		case 4: p[0] = (char)mmrand(rnd, 1, 0x0f); break;
		case 3: p[0] = (char)mmrand(rnd, 1, 0x07); break;
		case 2: p[0] = (char)mmrand(rnd, 1, 0x03); break;
		case 1: p[0] = 1; break;
		}
		value->size = 1;
		return;
	}

	/*
	 * WiredTiger doesn't store zero-length data items in row-store files,
	 * test that by inserting a zero-length data item every so often.
	 */
	if (keyno % 63 == 0) {
		p[0] = '\0';
		value->size = 0;
		return;
	}

	/*
	 * Data items have unique leading numbers by default and random lengths;
	 * variable-length column-stores use a duplicate data value to test RLE.
	 */
	if (g.type == VAR && mmrand(rnd, 1, 100) < g.c_repeat_data_pct) {
		(void)strcpy(p, "DUPLICATEV");
		p[10] = '/';
		value->size = val_dup_data_len;
	} else {
		(void)sprintf(p, "%010" PRIu64, keyno);
		p[10] = '/';
		value->size =
		    value_len(rnd, keyno, g.c_value_min, g.c_value_max);
	}
}

void
track(const char *tag, uint64_t cnt, TINFO *tinfo)
{
	static int lastlen = 0;
	int len;
	char msg[128];

	if (g.c_quiet || tag == NULL)
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
		testutil_die(EIO, "printf");
	if (fflush(stdout) == EOF)
		testutil_die(errno, "fflush");
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

	/* Primary backup directory. */
	len = strlen(g.home) + strlen("BACKUP") + 2;
	g.home_backup = dmalloc(len);
	snprintf(g.home_backup, len, "%s/%s", g.home, "BACKUP");

	/*
	 * Backup directory initialize command, remove and re-create the primary
	 * backup directory, plus a copy we maintain for recovery testing.
	 */
#undef	CMD
#ifdef _WIN32
#define	CMD	"del %s/%s %s/%s /s /q >:nul && mkdir %s/%s %s/%s"
#else
#define	CMD	"rm -rf %s/%s %s/%s && mkdir %s/%s %s/%s"
#endif
	len = strlen(g.home) * 4 +
	    strlen("BACKUP") * 2 + strlen("BACKUP_COPY") * 2 + strlen(CMD) + 1;
	g.home_backup_init = dmalloc(len);
	snprintf(g.home_backup_init, len, CMD,
	    g.home, "BACKUP", g.home, "BACKUP_COPY",
	    g.home, "BACKUP", g.home, "BACKUP_COPY");

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
			testutil_die(errno, "random number log");
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
		testutil_die(errno, "fclose");
	return;
}
