/*-
 * Public Domain 2014-present MongoDB, Inc.
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
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

void
key_init(void)
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
    for (i = 0; i < sizeof(g.key_rand_len) / sizeof(g.key_rand_len[0]); ++i) {
        max = g.c_key_max;
        if (i % 20 != 0 && max > g.c_key_min + 20)
            max = g.c_key_min + 20;
        g.key_rand_len[i] = mmrand(NULL, g.c_key_min, max);
    }
}

void
key_gen_init(WT_ITEM *key)
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

void
key_gen_teardown(WT_ITEM *key)
{
    free(key->mem);
    memset(key, 0, sizeof(*key));
}

static void
key_gen_common(WT_ITEM *key, uint64_t keyno, const char *const suffix)
{
    int len;
    char *p;

    p = key->mem;

    /*
     * The key always starts with a 10-digit string (the specified row) followed by two digits, a
     * random number between 1 and 15 if it's an insert, otherwise 00.
     */
    u64_to_string_zf(keyno, key->mem, 11);
    p[10] = '.';
    p[11] = suffix[0];
    p[12] = suffix[1];
    len = 13;

    /*
     * In a column-store, the key isn't used, it doesn't need a random length.
     */
    if (g.type == ROW) {
        p[len] = '/';

        /*
         * Because we're doing table lookup for key sizes, we weren't able to set really big keys
         * sizes in the table, the table isn't big enough to keep our hash from selecting too many
         * big keys and blowing out the cache. Handle that here, use a really big key 1 in 2500
         * times.
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
    key_gen_common(key, keyno, "00");
}

void
key_gen_insert(WT_RAND_STATE *rnd, WT_ITEM *key, uint64_t keyno)
{
    static const char *const suffix[15] = {
      "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13", "14", "15"};

    key_gen_common(key, keyno, suffix[mmrand(rnd, 0, 14)]);
}

static char *val_base;            /* Base/original value */
static uint32_t val_dup_data_len; /* Length of duplicate data items */
static uint32_t val_len;          /* Length of data items */

static inline uint32_t
value_len(WT_RAND_STATE *rnd, uint64_t keyno, uint32_t min, uint32_t max)
{
    /*
     * Focus on relatively small items, admitting the possibility of larger items. Pick a size close
     * to the minimum most of the time, only create a larger item 1 in 20 times, and a really big
     * item 1 in somewhere around 2500 items.
     */
    if (keyno % 2500 == 0 && max < KILOBYTE(80)) {
        min = KILOBYTE(80);
        max = KILOBYTE(100);
    } else if (keyno % 20 != 0 && max > min + 20)
        max = min + 20;
    return (mmrand(rnd, min, max));
}

void
val_init(void)
{
    size_t i;

    /*
     * Set initial buffer contents to recognizable text.
     *
     * Add a few extra bytes in order to guarantee we can always offset into the buffer by a few
     * extra bytes, used to generate different data for column-store run-length encoded files.
     */
    val_len = MAX(KILOBYTE(100), g.c_value_max) + 20;
    val_base = dmalloc(val_len);
    for (i = 0; i < val_len; ++i)
        val_base[i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[i % 26];

    val_dup_data_len = value_len(NULL, (uint64_t)mmrand(NULL, 1, 20), g.c_value_min, g.c_value_max);
}

void
val_teardown(void)
{
    free(val_base);
    val_base = NULL;
    val_dup_data_len = val_len = 0;
}

void
val_gen_init(WT_ITEM *value)
{
    value->mem = dmalloc(val_len);
    value->memsize = val_len;
    value->data = value->mem;
    value->size = 0;
}

void
val_gen_teardown(WT_ITEM *value)
{
    free(value->mem);
    memset(value, 0, sizeof(*value));
}

void
val_gen(WT_RAND_STATE *rnd, WT_ITEM *value, uint64_t keyno)
{
    char *p;

    p = value->mem;
    value->data = value->mem;

    /*
     * Fixed-length records: take the low N bits from the last digit of the record number.
     */
    if (g.type == FIX) {
        switch (g.c_bitcnt) {
        case 8:
            p[0] = (char)mmrand(rnd, 1, 0xff);
            break;
        case 7:
            p[0] = (char)mmrand(rnd, 1, 0x7f);
            break;
        case 6:
            p[0] = (char)mmrand(rnd, 1, 0x3f);
            break;
        case 5:
            p[0] = (char)mmrand(rnd, 1, 0x1f);
            break;
        case 4:
            p[0] = (char)mmrand(rnd, 1, 0x0f);
            break;
        case 3:
            p[0] = (char)mmrand(rnd, 1, 0x07);
            break;
        case 2:
            p[0] = (char)mmrand(rnd, 1, 0x03);
            break;
        case 1:
            p[0] = 1;
            break;
        }
        value->size = 1;
        return;
    }

    /*
     * WiredTiger doesn't store zero-length data items in row-store files, test that by inserting a
     * zero-length data item every so often.
     */
    if (keyno % 63 == 0) {
        p[0] = '\0';
        value->size = 0;
        return;
    }

    /*
     * Data items have unique leading numbers by default and random lengths; variable-length
     * column-stores use a duplicate data value to test RLE.
     */
    if (g.type == VAR && mmrand(rnd, 1, 100) < g.c_repeat_data_pct) {
        value->size = val_dup_data_len;
        memcpy(p, val_base, value->size);
        (void)strcpy(p, "DUPLICATEV");
        p[10] = '/';
    } else {
        value->size = value_len(rnd, keyno, g.c_value_min, g.c_value_max);
        memcpy(p, val_base, value->size);
        u64_to_string_zf(keyno, p, 11);
        p[10] = '/';
    }
}

void
track(const char *tag, uint64_t cnt, TINFO *tinfo)
{
    static size_t lastlen = 0;
    size_t len;
    char msg[128];

    if (g.c_quiet || tag == NULL)
        return;

    if (tinfo == NULL && cnt == 0)
        testutil_check(
          __wt_snprintf_len_set(msg, sizeof(msg), &len, "%4" PRIu32 ": %s", g.run_cnt, tag));
    else if (tinfo == NULL)
        testutil_check(__wt_snprintf_len_set(
          msg, sizeof(msg), &len, "%4" PRIu32 ": %s: %" PRIu64, g.run_cnt, tag, cnt));
    else
        testutil_check(__wt_snprintf_len_set(msg, sizeof(msg), &len, "%4" PRIu32 ": %s: "
                                                                     "search %" PRIu64 "%s, "
                                                                     "insert %" PRIu64 "%s, "
                                                                     "update %" PRIu64 "%s, "
                                                                     "remove %" PRIu64 "%s",
          g.run_cnt, tag, tinfo->search > M(9) ? tinfo->search / M(1) : tinfo->search,
          tinfo->search > M(9) ? "M" : "",
          tinfo->insert > M(9) ? tinfo->insert / M(1) : tinfo->insert,
          tinfo->insert > M(9) ? "M" : "",
          tinfo->update > M(9) ? tinfo->update / M(1) : tinfo->update,
          tinfo->update > M(9) ? "M" : "",
          tinfo->remove > M(9) ? tinfo->remove / M(1) : tinfo->remove,
          tinfo->remove > M(9) ? "M" : ""));

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
 *     Build the standard paths and shell commands we use.
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
    testutil_check(__wt_snprintf(g.home_log, len, "%s/%s", g.home, "log"));

    /* Page dump file. */
    len = strlen(g.home) + strlen("pagedump") + 2;
    g.home_pagedump = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_pagedump, len, "%s/%s", g.home, "pagedump"));

    /* RNG log file. */
    len = strlen(g.home) + strlen("rand") + 2;
    g.home_rand = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_rand, len, "%s/%s", g.home, "rand"));

    /* Run file. */
    len = strlen(g.home) + strlen("CONFIG") + 2;
    g.home_config = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_config, len, "%s/%s", g.home, "CONFIG"));

    /* Statistics file. */
    len = strlen(g.home) + strlen("stats") + 2;
    g.home_stats = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_stats, len, "%s/%s", g.home, "stats"));

/*
 * Home directory initialize command: create the directory if it doesn't exist, else remove
 * everything except the RNG log file.
 *
 * Redirect the "cd" command to /dev/null so chatty cd implementations don't add the new working
 * directory to our output.
 */
#undef CMD
#ifdef _WIN32
#define CMD                                             \
    "del /q rand.copy & "                               \
    "(IF EXIST %s\\rand copy /y %s\\rand rand.copy) & " \
    "(IF EXIST %s rd /s /q %s) & mkdir %s & "           \
    "(IF EXIST rand.copy copy rand.copy %s\\rand)"
    len = strlen(g.home) * 7 + strlen(CMD) + 1;
    g.home_init = dmalloc(len);
    testutil_check(
      __wt_snprintf(g.home_init, len, CMD, g.home, g.home, g.home, g.home, g.home, g.home, g.home));
#else
#define CMD                    \
    "test -e %s || mkdir %s; " \
    "cd %s > /dev/null && rm -rf `ls | sed /rand/d`"
    len = strlen(g.home) * 3 + strlen(CMD) + 1;
    g.home_init = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_init, len, CMD, g.home, g.home, g.home));
#endif

    /* Primary backup directory. */
    len = strlen(g.home) + strlen("BACKUP") + 2;
    g.home_backup = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_backup, len, "%s/%s", g.home, "BACKUP"));

/*
 * Backup directory initialize command, remove and re-create the primary backup directory, plus a
 * copy we maintain for recovery testing.
 */
#undef CMD
#ifdef _WIN32
#define CMD "rd /s /q %s\\%s %s\\%s & mkdir %s\\%s %s\\%s"
#else
#define CMD "rm -rf %s/%s %s/%s && mkdir %s/%s %s/%s"
#endif
    len = strlen(g.home) * 4 + strlen("BACKUP") * 2 + strlen("BACKUP_COPY") * 2 + strlen(CMD) + 1;
    g.home_backup_init = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_backup_init, len, CMD, g.home, "BACKUP", g.home,
      "BACKUP_COPY", g.home, "BACKUP", g.home, "BACKUP_COPY"));

/*
 * Salvage command, save the interesting files so we can replay the salvage command as necessary.
 *
 * Redirect the "cd" command to /dev/null so chatty cd implementations don't add the new working
 * directory to our output.
 */
#undef CMD
#ifdef _WIN32
#define CMD                                    \
    "cd %s && "                                \
    "rd /q /s slvg.copy & mkdir slvg.copy && " \
    "copy WiredTiger* slvg.copy\\ >:nul && copy wt* slvg.copy\\ >:nul"
#else
#define CMD                                   \
    "cd %s > /dev/null && "                   \
    "rm -rf slvg.copy && mkdir slvg.copy && " \
    "cp WiredTiger* wt* slvg.copy/"
#endif
    len = strlen(g.home) + strlen(CMD) + 1;
    g.home_salvage_copy = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_salvage_copy, len, CMD, g.home));
}

/*
 * rng --
 *     Return a random number.
 */
uint32_t
rng(WT_RAND_STATE *rnd)
{
    u_long ulv;
    uint32_t v;
    char *endptr, buf[64];

    /*
     * Threaded operations have their own RNG information, otherwise we use the default.
     */
    if (rnd == NULL)
        rnd = &g.rnd;

    /*
     * We can reproduce a single-threaded run based on the random numbers used in the initial run,
     * plus the configuration files.
     *
     * Check g.replay and g.rand_log_stop: multithreaded runs log/replay until they get to the
     * operations phase, then turn off log/replay, threaded operation order can't be replayed.
     */
    if (g.rand_log_stop)
        return (__wt_random(rnd));

    if (g.replay) {
        if (fgets(buf, sizeof(buf), g.randfp) == NULL) {
            if (feof(g.randfp)) {
                fprintf(stderr,
                  "\n"
                  "end of random number log reached\n");
                exit(EXIT_SUCCESS);
            }
            testutil_die(errno, "random number log");
        }

        errno = 0;
        ulv = strtoul(buf, &endptr, 10);
        testutil_assert(errno == 0 && endptr[0] == '\n');
        testutil_assert(ulv <= UINT32_MAX);
        return ((uint32_t)ulv);
    }

    v = __wt_random(rnd);

    /* Save and flush the random number so we're up-to-date on error. */
    (void)fprintf(g.randfp, "%" PRIu32 "\n", v);
    (void)fflush(g.randfp);

    return (v);
}

/*
 * fclose_and_clear --
 *     Close a file and clear the handle so we don't close twice.
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

/*
 * checkpoint --
 *     Periodically take a checkpoint
 */
WT_THREAD_RET
checkpoint(void *arg)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    u_int secs;
    char config_buf[64];
    const char *ckpt_config;
    bool backup_locked;

    (void)arg;
    conn = g.wts_conn;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    for (secs = mmrand(NULL, 1, 10); !g.workers_finished;) {
        if (secs > 0) {
            __wt_sleep(1, 0);
            --secs;
            continue;
        }

        /*
         * LSM and data-sources don't support named checkpoints. Also, don't attempt named
         * checkpoints during a hot backup. It's OK to create named checkpoints during a hot backup,
         * but we can't delete them, so repeating an already existing named checkpoint will fail
         * when we can't drop the previous one.
         */
        ckpt_config = NULL;
        backup_locked = false;
        if (!DATASOURCE("lsm"))
            switch (mmrand(NULL, 1, 20)) {
            case 1:
                /*
                 * 5% create a named snapshot. Rotate between a
                 * few names to test multiple named snapshots in
                 * the system.
                 */
                ret = pthread_rwlock_trywrlock(&g.backup_lock);
                if (ret == 0) {
                    backup_locked = true;
                    testutil_check(__wt_snprintf(
                      config_buf, sizeof(config_buf), "name=mine.%" PRIu32, mmrand(NULL, 1, 4)));
                    ckpt_config = config_buf;
                } else if (ret != EBUSY)
                    testutil_check(ret);
                break;
            case 2:
                /*
                 * 5% drop all named snapshots.
                 */
                ret = pthread_rwlock_trywrlock(&g.backup_lock);
                if (ret == 0) {
                    backup_locked = true;
                    ckpt_config = "drop=(all)";
                } else if (ret != EBUSY)
                    testutil_check(ret);
                break;
            }

        testutil_check(session->checkpoint(session, ckpt_config));

        if (backup_locked)
            testutil_check(pthread_rwlock_unlock(&g.backup_lock));

        secs = mmrand(NULL, 5, 40);
    }

    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * timestamp --
 *     Periodically update the oldest timestamp.
 */
WT_THREAD_RET
timestamp(void *arg)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    char buf[64];
    bool done;

    (void)(arg);
    conn = g.wts_conn;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s", "oldest_timestamp="));

    /* Update the oldest timestamp at least once every 15 seconds. */
    done = false;
    do {
        /*
         * Do a final bump of the oldest timestamp as part of shutting down the worker threads,
         * otherwise recent operations can prevent verify from running.
         */
        if (g.workers_finished)
            done = true;
        else
            random_sleep(&g.rnd, 15);

        /*
         * Lock out transaction timestamp operations. The lock acts as a barrier ensuring we've
         * checked if the workers have finished, we don't want that line reordered.
         */
        testutil_check(pthread_rwlock_wrlock(&g.ts_lock));

        ret = conn->query_timestamp(conn, buf + strlen("oldest_timestamp="), "get=all_committed");
        testutil_assert(ret == 0 || ret == WT_NOTFOUND);
        if (ret == 0)
            testutil_check(conn->set_timestamp(conn, buf));

        testutil_check(pthread_rwlock_unlock(&g.ts_lock));
    } while (!done);

    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * alter --
 *     Periodically alter a table's metadata.
 */
WT_THREAD_RET
alter(void *arg)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    u_int period;
    char buf[32];
    bool access_value;

    (void)(arg);
    conn = g.wts_conn;

    /*
     * Only alter the access pattern hint. If we alter the cache resident setting we may end up with
     * a setting that fills cache and doesn't allow it to be evicted.
     */
    access_value = false;

    /* Open a session */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    while (!g.workers_finished) {
        period = mmrand(NULL, 1, 10);

        testutil_check(__wt_snprintf(
          buf, sizeof(buf), "access_pattern_hint=%s", access_value ? "random" : "none"));
        access_value = !access_value;
        /*
         * Alter can return EBUSY if concurrent with other operations.
         */
        while ((ret = session->alter(session, g.uri, buf)) != 0 && ret != EBUSY)
            testutil_die(ret, "session.alter");
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
    }

    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}
