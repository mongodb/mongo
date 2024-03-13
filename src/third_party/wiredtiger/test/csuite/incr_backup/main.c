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

/*
 * This program tests incremental backup in a randomized way. The random seed used is reported and
 * can be used in another run.
 */

#include "test_util.h"

#include <sys/wait.h>
#include <signal.h>

#define BACKUP_RETAIN 4
#define BACKUP_SRC "backup.src."

#define ITERATIONS 10
#define MAX_NTABLES 100

#define MAX_KEY_SIZE 100
#define MAX_VALUE_SIZE (10 * WT_THOUSAND)
#define MAX_MODIFY_ENTRIES 10
#define MAX_MODIFY_DIFF 500

#define URI_MAX_LEN 32
#define URI_FORMAT "table:t%d-%d"
#define KEY_FORMAT "key-%d-%d"
#define TABLE_FORMAT "key_format=S,value_format=u"

#define CONN_CONFIG_COMMON                                                                        \
    "timing_stress_for_test=[backup_rename],statistics=(all),statistics_log=(json,on_close,wait=" \
    "1)"

#define NUM_ALLOC 5
static const char *alloc_sizes[] = {"512B", "8K", "64K", "1M", "16M"};

static int verbose_level = 0;
static uint64_t seed = 0;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

static bool do_drop = true;

#define VERBOSE(level, fmt, ...)      \
    do {                              \
        if (level <= verbose_level)   \
            printf(fmt, __VA_ARGS__); \
    } while (0)

/*
 * We keep an array of tables, each one may or may not be in use. "In use" means it has been
 * created, and will be updated from time to time.
 */
typedef struct {
    char *name;            /* non-null entries represent tables in use */
    uint32_t name_index;   /* bumped when we drop, so we get unique names. */
    uint64_t change_count; /* number of changes so far to the table */
    WT_RAND_STATE rand;
    uint32_t max_value_size;
} TABLE;
#define TABLE_VALID(tablep) ((tablep)->name != NULL)

/*
 * The set of all tables in play, and other information used for this run.
 */
typedef struct {
    TABLE *table;           /* set of potential tables */
    uint32_t table_count;   /* size of table array */
    uint32_t tables_in_use; /* count of tables that exist */
    uint32_t full_backup_number;
    uint32_t incr_backup_number;
} TABLE_INFO;

extern int __wt_optind;
extern char *__wt_optarg;

/*
 * The choices of operations we do to each table. Please do not initialize enum elements with custom
 * values as there's an assumption that the first element has the default value of 0 and the last
 * element is always reserved to check count on elements.
 */
typedef enum { INSERT, MODIFY, REMOVE, UPDATE, _OPERATION_TYPE_COUNT } OPERATION_TYPE;

/*
 * Cycle of changes to a table.
 *
 * When making changes to a table, the first KEYS_PER_TABLE changes are all inserts, the next
 * KEYS_PER_TABLE are updates of the same records. The next KEYS_PER_TABLE are modifications of
 * existing records, and the last KEYS_PER_TABLE will be removes. This defines one "cycle", and
 * CHANGES_PER_CYCLE is the number of changes in a complete cycle. Thus at the end/beginning of each
 * cycle, there are zero keys in the table.
 *
 * Having a predictable cycle makes it easy on the checking side (knowing how many total changes
 * have been made) to check the state of the table.
 */
#define KEYS_PER_TABLE (10 * WT_THOUSAND)
#define CHANGES_PER_CYCLE (KEYS_PER_TABLE * _OPERATION_TYPE_COUNT)

/*
 * usage --
 *     Print usage message and exit.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir] [-S seed] [-v verbose_level]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * die --
 *     Called when testutil_assert or testutil_check fails.
 */
static void
die(void)
{
    fprintf(stderr,
      "**** FAILURE\n"
      "To reproduce, please rerun with: %s -S %" PRIu64 "\n",
      progname, seed);
}

/*
 * get_operation_type --
 *     Get operation type based on the number of changes.
 */
static OPERATION_TYPE
get_operation_type(uint64_t change_count)
{
    int32_t op_type;

    op_type = ((change_count % CHANGES_PER_CYCLE) / KEYS_PER_TABLE);
    testutil_assert(op_type <= _OPERATION_TYPE_COUNT);

    return (OPERATION_TYPE)op_type;
}

/*
 * key_value --
 *     Return the key, value and operation type for a given change to a table. See "Cycle of changes
 *     to a table" above.
 *
 * The keys generated are unique among the 10000, but we purposely don't make them sequential, so
 *     that insertions tend to be scattered among the pages in the B-tree.
 *
 * "key-0-0", "key-1-0", "key-2-0""... "key-99-0", "key-0-1", "key-1-1", ...
 */
static void
key_value(uint64_t change_count, char *key, size_t key_size, WT_ITEM *item, OPERATION_TYPE *typep)
{
    uint32_t key_num;
    OPERATION_TYPE op_type;
    size_t pos, value_size;
    char *cp;
    char ch;

    key_num = change_count % KEYS_PER_TABLE;
    *typep = op_type = get_operation_type(change_count);

    testutil_snprintf(key, key_size, KEY_FORMAT, (int)(key_num % 100), (int)(key_num / 100));
    if (op_type == REMOVE)
        return; /* remove needs no key */

    /* The value sizes vary "predictably" up to the max value size for this table. */
    value_size = (change_count * 103) % (item->size + 1);
    testutil_assert(value_size <= item->size);

    /*
     * For a given key, a value is first inserted, then later updated, then modified. When a value
     * is inserted, it is all the letter 'a'. When the value is updated, is it mostly 'b', with some
     * 'c' mixed in. When the value is to modified, we'll end up with a value with mostly 'b' and
     * 'M' mixed in, in different spots. Thus the modify operation will have both additions ('M')
     * and subtractions ('c') from the previous version.
     */
    if (op_type == INSERT)
        ch = 'a';
    else
        ch = 'b';

    cp = (char *)item->data;
    for (pos = 0; pos < value_size; pos++) {
        cp[pos] = ch;
        if (op_type == UPDATE && ((50 < pos && pos < 60) || (150 < pos && pos < 160)))
            cp[pos] = 'c';
        else if (op_type == MODIFY && ((20 < pos && pos < 30) || (120 < pos && pos < 130)))
            cp[pos] = 'M';
    }
    item->size = value_size;
}

/*
 * table_changes --
 *     Potentially make changes to a single table.
 */
static void
table_changes(WT_SESSION *session, TABLE *table)
{
    WT_CURSOR *cur;
    WT_ITEM item, item2;
    WT_MODIFY modify_entries[MAX_MODIFY_ENTRIES];
    OPERATION_TYPE op_type;
    uint64_t change_count;
    uint32_t i, nrecords;
    int modify_count;
    u_char *value, *value2;
    char key[MAX_KEY_SIZE];

    /*
     * We change each table in use about half the time.
     */
    if (__wt_random(&table->rand) % 2 == 0) {
        value = dcalloc(1, table->max_value_size);
        value2 = dcalloc(1, table->max_value_size);
        nrecords = __wt_random(&table->rand) % WT_THOUSAND;
        VERBOSE(4, "changing %" PRIu32 " records in %s\n", nrecords, table->name);
        testutil_check(session->open_cursor(session, table->name, NULL, NULL, &cur));
        for (i = 0; i < nrecords; i++) {
            change_count = table->change_count++;
            item.data = value;
            item.size = table->max_value_size;
            key_value(change_count, key, sizeof(key), &item, &op_type);
            cur->set_key(cur, key);

            /*
             * To satisfy code analysis checks, we must handle all elements of the enum in the
             * switch statement.
             */
            switch (op_type) {
            case INSERT:
                cur->set_value(cur, &item);
                testutil_check(cur->insert(cur));
                break;
            case MODIFY:
                item2.data = value2;
                item2.size = table->max_value_size;
                key_value(change_count - KEYS_PER_TABLE, NULL, 0, &item2, &op_type);
                modify_count = MAX_MODIFY_ENTRIES;
                testutil_check(wiredtiger_calc_modify(
                  session, &item2, &item, MAX_MODIFY_DIFF, modify_entries, &modify_count));
                testutil_check(cur->modify(cur, modify_entries, modify_count));
                break;
            case REMOVE:
                testutil_check(cur->remove(cur));
                break;
            case UPDATE:
                cur->set_value(cur, &item);
                testutil_check(cur->update(cur));
                break;
            case _OPERATION_TYPE_COUNT:
                testutil_die(0, "Unexpected OPERATION_TYPE: _OPERATION_TYPE_COUNT");
            }
        }
        free(value);
        free(value2);
        testutil_check(cur->close(cur));
    }
}

/*
 * create_table --
 *     Create a table for the given slot.
 */
static void
create_table(WT_SESSION *session, WT_RAND_STATE *rand, TABLE_INFO *tinfo, uint32_t slot)
{
    uint32_t alloc;
    char buf[4096], *uri;
    const char *allocstr;

    testutil_assert(!TABLE_VALID(&tinfo->table[slot]));
    uri = dcalloc(1, URI_MAX_LEN);
    testutil_snprintf(
      uri, URI_MAX_LEN, URI_FORMAT, (int)slot, (int)tinfo->table[slot].name_index++);

    /*
     * A quarter of the time use a non-default allocation size on the table. This is set
     * independently of the granularity to stress mismatched values.
     */
    if (__wt_random(rand) % 4 == 0) {
        alloc = __wt_random(rand) % NUM_ALLOC;
        allocstr = alloc_sizes[alloc];
        testutil_snprintf(buf, sizeof(buf),
          "%s,allocation_size=%s,internal_page_max=%s,leaf_page_max=%s", TABLE_FORMAT, allocstr,
          allocstr, allocstr);
    } else
        testutil_snprintf(buf, sizeof(buf), "%s", TABLE_FORMAT);
    VERBOSE(3, "create %s: %s\n", uri, buf);
    testutil_check(session->create(session, uri, buf));
    tinfo->table[slot].name = uri;
    tinfo->tables_in_use++;
}

/*
 * drop_table --
 *     TODO: Add a comment describing this function.
 */
static void
drop_table(WT_SESSION *session, TABLE_INFO *tinfo, uint32_t slot)
{
    char *uri;

    testutil_assert(TABLE_VALID(&tinfo->table[slot]));
    uri = tinfo->table[slot].name;

    VERBOSE(3, "drop %s\n", uri);
    WT_OP_CHECKPOINT_WAIT(session, session->drop(session, uri, NULL));
    free(uri);
    tinfo->table[slot].name = NULL;
    tinfo->table[slot].change_count = 0;
    tinfo->tables_in_use--;
}

/*
 * tables_free --
 *     Free the list of tables.
 */
static void
tables_free(TABLE_INFO *tinfo)
{
    uint32_t slot;

    for (slot = 0; slot < tinfo->table_count; slot++) {
        if (tinfo->table[slot].name != NULL) {
            free(tinfo->table[slot].name);
            tinfo->table[slot].name = NULL;
        }
    }
    free(tinfo->table);
    tinfo->table = NULL;
}

/*
 * base_backup --
 *     TODO: Add a comment describing this function.
 */
static void
base_backup(WT_CONNECTION *conn, WT_RAND_STATE *rand, const char *home, TABLE_INFO *tinfo)
{
    uint32_t granularity, granularity_kb;
    int id, nfiles;
    bool consolidate;

    nfiles = 0;
    id = (int)tinfo->full_backup_number;

    /* Half of the runs with very low granularity to stress bitmaps */
    granularity = __wt_random(rand) % 20;
    if (__wt_random(rand) % 2 == 0) {
        granularity += 4;
        granularity_kb = granularity;
    } else {
        granularity += 1;
        granularity_kb = granularity * 1024;
    }
    if (__wt_random(rand) % 2 == 0)
        consolidate = true;
    else
        consolidate = false;
    /* Use the same ID for the directory name and configuration */
    testutil_backup_create_full(conn, home, id, consolidate, granularity_kb, &nfiles);
    VERBOSE(2, " finished base backup: %d files\n", nfiles);
}

/*
 * incr_backup --
 *     Perform an incremental backup into an existing backup directory.
 */
static void
incr_backup(WT_CONNECTION *conn, const char *home, TABLE_INFO *tinfo)
{
    int nfiles, nranges, num_modified;

    VERBOSE(2, "INCREMENTAL BACKUP: START: %" PRIu32 " source=%" PRIu32 "\n",
      tinfo->incr_backup_number, tinfo->full_backup_number);

    nfiles = nranges = num_modified = 0;
    testutil_backup_create_incremental(conn, home, (int)tinfo->incr_backup_number,
      (int)tinfo->full_backup_number, true /* verbose */, &nfiles, &nranges, &num_modified);
    VERBOSE(2,
      "INCREMENTAL BACKUP: COMPLETE: %" PRIu32 " files=%" PRId32 ", ranges=%" PRId32
      ", unmodified=%" PRId32 "\n",
      tinfo->incr_backup_number, nfiles, nranges, num_modified);
}

/*
 * check_table --
 *     TODO: Add a comment describing this function.
 */
static void
check_table(WT_SESSION *session, TABLE *table)
{
    WT_CURSOR *cursor;
    WT_ITEM item, got_value;
    OPERATION_TYPE op_type;
    uint64_t boundary, change_count, expect_records, got_records, total_changes;
    int keylow, keyhigh, ret;
    u_char *value;
    char *got_key;
    char key[MAX_KEY_SIZE];

    expect_records = 0;
    total_changes = table->change_count;
    boundary = total_changes % KEYS_PER_TABLE;
    op_type = get_operation_type(total_changes);
    value = dcalloc(1, table->max_value_size);

    VERBOSE(3, "Checking: %s\n", table->name);

    /*
     * To satisfy code analysis checks, we must handle all elements of the enum in the switch
     * statement.
     */
    switch (op_type) {
    case INSERT:
        expect_records = total_changes % KEYS_PER_TABLE;
        break;
    case MODIFY:
    case UPDATE:
        expect_records = KEYS_PER_TABLE;
        break;
    case REMOVE:
        expect_records = KEYS_PER_TABLE - (total_changes % KEYS_PER_TABLE);
        break;
    case _OPERATION_TYPE_COUNT:
        testutil_die(0, "Unexpected OPERATION_TYPE: _OPERATION_TYPE_COUNT");
    }

    testutil_check(session->open_cursor(session, table->name, NULL, NULL, &cursor));
    got_records = 0;
    while ((ret = cursor->next(cursor)) == 0) {
        got_records++;
        testutil_check(cursor->get_key(cursor, &got_key));
        testutil_check(cursor->get_value(cursor, &got_value));

        /*
         * Reconstruct the change number from the key. See key_value() for details on how the key is
         * constructed.
         */
        testutil_assert(sscanf(got_key, KEY_FORMAT, &keylow, &keyhigh) == 2);
        change_count = (u_int)keyhigh * 100 + (u_int)keylow;
        item.data = value;
        item.size = table->max_value_size;
        if (op_type == INSERT || (op_type == UPDATE && change_count < boundary))
            change_count += 0;
        else if (op_type == UPDATE || (op_type == MODIFY && change_count < boundary))
            change_count += KEYS_PER_TABLE;
        else if (op_type == MODIFY || (op_type == REMOVE && change_count < boundary))
            change_count += 20 * WT_THOUSAND;
        else
            testutil_assert(false);
        key_value(change_count, key, sizeof(key), &item, &op_type);
        testutil_assert(strcmp(key, got_key) == 0);
        testutil_assert(got_value.size == item.size);
        testutil_assert(memcmp(got_value.data, item.data, item.size) == 0);
    }
    testutil_assert(got_records == expect_records);
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
    free(value);
}

/*
 * check_backup --
 *     Verify the backup to make sure the proper tables exist and have the correct content.
 */
static void
check_backup(uint32_t backup_iter, TABLE_INFO *tinfo)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    uint32_t slot;
    char backup_check[PATH_MAX], backup_home[PATH_MAX];

    /*
     * Generate the names for the backup home directory and the temporary backup directory for
     * verification.
     */
    testutil_snprintf(backup_check, sizeof(backup_check), CHECK_BASE "%" PRIu32, backup_iter);
    testutil_snprintf(backup_home, sizeof(backup_home), BACKUP_BASE "%" PRIu32, backup_iter);

    VERBOSE(
      2, "CHECK BACKUP: copy %s to %s, then check %s\n", backup_home, backup_check, backup_check);

    testutil_remove(backup_check);
    testutil_copy(backup_home, backup_check);

    testutil_check(wiredtiger_open(backup_check, NULL, CONN_CONFIG_COMMON, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    for (slot = 0; slot < tinfo->table_count; slot++) {
        if (TABLE_VALID(&tinfo->table[slot]))
            check_table(session, &tinfo->table[slot]);
    }

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
    testutil_remove(backup_check);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TABLE_INFO tinfo;
    WT_CONNECTION *conn;
    WT_FILE_COPY_OPTS copy_opts;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    uint32_t file_max, iter, max_value_size, next_checkpoint, rough_size, slot;
    int ch, ncheckpoints, nreopens;
    const char *backup_verbose, *working_dir;
    char backup_src[1024], conf[1024], home[1024];
    bool preserve;

    preserve = false;
    ncheckpoints = nreopens = 0;
    (void)testutil_set_progname(argv);
    custom_die = die; /* Set our own abort handler */
    WT_CLEAR(tinfo);

    memset(&copy_opts, 0, sizeof(copy_opts));
    copy_opts.preserve = true;

    working_dir = "WT_TEST.incr_backup";

    while ((ch = __wt_getopt(progname, argc, argv, "h:pS:v:")) != EOF)
        switch (ch) {
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'p':
            preserve = true;
            break;
        case 'S':
            seed = (uint64_t)atoll(__wt_optarg);
            break;
        case 'v':
            verbose_level = atoi(__wt_optarg);
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    if (seed == 0) {
        __wt_random_init_seed(NULL, &rnd);
        seed = rnd.v;
    } else
        rnd.v = seed;

    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    printf("Seed: %" PRIu64 "\n", seed);

    testutil_recreate_dir(home);
    /*
     * Go inside the home directory and create the database home. We also use the home directory as
     * the top level for creating the backup directories and check directory.
     */
    if (chdir(home) != 0)
        testutil_die(errno, "parent chdir: %s", home);
    testutil_recreate_dir(WT_HOME_DIR);

    backup_verbose = (verbose_level >= 4) ? "verbose=(backup)" : "";

    /*
     * We create an overall max_value_size.  From that, we'll set a random max_value_size per table.
     * In addition, individual values put into each table vary randomly in size, up to the
     * max_value_size of the table.
     * This tends to make sure that 1) each table has a "personality" of size ranges within it
     * 2) there are some runs that tend to have a lot more data than other runs.  If we made every
     * insert choose a uniform random size between 1 and MAX_VALUE_SIZE, once we did a bunch
     * of inserts, each run would look very much the same with respect to value size.
     */
    max_value_size = __wt_random(&rnd) % MAX_VALUE_SIZE;

    /* Compute a random value of file_max. */
    rough_size = __wt_random(&rnd) % 3;
    if (rough_size == 0)
        file_max = 100 + __wt_random(&rnd) % 100; /* small log files, min 100K */
    else if (rough_size == 1)
        file_max = 200 + __wt_random(&rnd) % WT_THOUSAND; /* 200K to ~1M */
    else
        file_max = WT_THOUSAND + __wt_random(&rnd) % (20 * WT_THOUSAND); /* 1M to ~20M */
    testutil_snprintf(conf, sizeof(conf), "%s,create,%s,log=(enabled=true,file_max=%" PRIu32 "K)",
      CONN_CONFIG_COMMON, backup_verbose, file_max);
    VERBOSE(2, "wiredtiger config: %s\n", conf);
    testutil_check(wiredtiger_open(WT_HOME_DIR, NULL, conf, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    tinfo.table_count = __wt_random(&rnd) % MAX_NTABLES + 1;
    tinfo.table = dcalloc(tinfo.table_count, sizeof(tinfo.table[0]));

    /*
     * Give each table its own random generator. This makes it easier to simplify a failing test to
     * use fewer tables, but have those just tables behave the same.
     */
    for (slot = 0; slot < tinfo.table_count; slot++) {
        tinfo.table[slot].rand.v = seed + slot;
        testutil_assert(!TABLE_VALID(&tinfo.table[slot]));
        tinfo.table[slot].max_value_size = __wt_random(&rnd) % (max_value_size + 1);
    }

    /* How many files should we update until next checkpoint. */
    next_checkpoint = __wt_random(&rnd) % tinfo.table_count;

    for (iter = 0; iter < ITERATIONS; iter++) {
        VERBOSE(1, "**** iteration %" PRIu32 " ****\n", iter);

        /*
         * We have schema changes during about half the iterations. The number of schema changes
         * varies, averaging 10.
         */
        if (tinfo.tables_in_use == 0 || __wt_random(&rnd) % 2 != 0) {
            while (__wt_random(&rnd) % 10 != 0) {
                /*
                 * For schema events, we choose to create or drop tables. We pick a random slot, and
                 * if it is empty, create a table there. Otherwise we drop. That should give us a
                 * steady state with slots mostly filled.
                 */
                slot = __wt_random(&rnd) % tinfo.table_count;
                if (!TABLE_VALID(&tinfo.table[slot]))
                    create_table(session, &rnd, &tinfo, slot);
                else if (do_drop)
                    drop_table(session, &tinfo, slot);
            }
        }
        for (slot = 0; slot < tinfo.table_count; slot++) {
            if (TABLE_VALID(&tinfo.table[slot]))
                table_changes(session, &tinfo.table[slot]);
            if (next_checkpoint-- == 0) {
                VERBOSE(2, "Checkpoint %d\n", ncheckpoints);
                testutil_check(session->checkpoint(session, NULL));
                next_checkpoint = __wt_random(&rnd) % tinfo.table_count;
                ncheckpoints++;
            }
        }
        /* Close and reopen the connection once in a while. */
        if (iter != 0 && __wt_random(&rnd) % 5 == 0) {
            VERBOSE(2, "Close and reopen the connection %d\n", nreopens);
            testutil_check(conn->close(conn, NULL));
            testutil_snprintf(backup_src, sizeof(backup_src), BACKUP_SRC "%" PRIu32, iter);
            /* Check the source bitmap after restart. Copy while closed. */
            testutil_copy_ext(WT_HOME_DIR, backup_src, &copy_opts);

            testutil_check(wiredtiger_open(WT_HOME_DIR, NULL, conf, &conn));
            testutil_check(conn->open_session(conn, NULL, NULL, &session));

            /* Test against the copied directory. */
            testutil_verify_src_backup(conn, backup_src, WT_HOME_DIR, NULL);
            testutil_remove(backup_src);
            nreopens++;
        }

        if (iter == 0) {
            VERBOSE(2, "Iteration %" PRIu32 ": taking full backup\n", iter);
            tinfo.full_backup_number = iter;
            base_backup(conn, &rnd, WT_HOME_DIR, &tinfo);
            check_backup(iter, &tinfo);
        } else {
            /* Randomly restart with a full backup again. */
            if (__wt_random(&rnd) % 10 == 0) {
                VERBOSE(2, "Iteration %" PRIu32 ": taking new full backup\n", iter);
                tinfo.full_backup_number = iter;
                base_backup(conn, &rnd, WT_HOME_DIR, &tinfo);
                check_backup(iter, &tinfo);
            } else {
                VERBOSE(2, "Iteration %" PRIu32 ": taking incremental backup\n", iter);
                tinfo.incr_backup_number = iter;
                incr_backup(conn, WT_HOME_DIR, &tinfo);
                check_backup(iter, &tinfo);
            }
        }
        testutil_delete_old_backups(BACKUP_RETAIN);
    }
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
    tables_free(&tinfo);

    printf("Success.\n");
    if (!preserve) {
        testutil_delete_old_backups(0);
        testutil_clean_test_artifacts(home);
        testutil_remove(home);
    }

    return (EXIT_SUCCESS);
}
