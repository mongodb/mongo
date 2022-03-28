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

#define ITERATIONS 10
#define MAX_NTABLES 100

#define MAX_KEY_SIZE 100
#define MAX_VALUE_SIZE 10000
#define MAX_MODIFY_ENTRIES 10
#define MAX_MODIFY_DIFF 500

#define URI_MAX_LEN 32
#define URI_FORMAT "table:t%d-%d"
#define KEY_FORMAT "key-%d-%d"
#define TABLE_FORMAT "key_format=S,value_format=u"

#define CONN_CONFIG_COMMON "timing_stress_for_test=[backup_rename]"

#define NUM_ALLOC 5
static const char *alloc_sizes[] = {"512B", "8K", "64K", "1M", "16M"};

static int verbose_level = 0;
static uint64_t seed = 0;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * Note: set this to true to copy incremental files completely.
 */
static bool slow_incremental = false;

static bool do_drop = true;
static bool do_rename = true;

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
    uint32_t name_index;   /* bumped when we rename or drop, so we get unique names. */
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

/*
 * The set of active files in a backup. This is our "memory" of files that are used in each backup,
 * so we can remove any that are not mentioned in the next backup.
 */
typedef struct {
    char **names;
    uint32_t count;
} ACTIVE_FILES;

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
#define KEYS_PER_TABLE 10000
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

    testutil_check(
      __wt_snprintf(key, key_size, KEY_FORMAT, (int)(key_num % 100), (int)(key_num / 100)));
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
 * active_files_init --
 *     Initialize (clear) the active file struct.
 */
static void
active_files_init(ACTIVE_FILES *active)
{
    WT_CLEAR(*active);
}

/*
 * active_files_print --
 *     Print the set of active files for debugging.
 */
static void
active_files_print(ACTIVE_FILES *active, const char *msg)
{
    uint32_t i;

    VERBOSE(6, "Active files: %s, %" PRIu32 " entries\n", msg, active->count);
    for (i = 0; i < active->count; i++)
        VERBOSE(6, "  %s\n", active->names[i]);
}

/*
 * active_files_add --
 *     Add a new name to the active file list.
 */
static void
active_files_add(ACTIVE_FILES *active, const char *name)
{
    uint32_t pos;

    pos = active->count++;
    active->names = drealloc(active->names, sizeof(char *) * active->count);
    active->names[pos] = strdup(name);
}

/*
 * active_files_sort_function --
 *     Sort function for qsort.
 */
static int
active_files_sort_function(const void *left, const void *right)
{
    return (strcmp(*(const char **)left, *(const char **)right));
}

/*
 * active_files_sort --
 *     Sort the list of names in the active file list.
 */
static void
active_files_sort(ACTIVE_FILES *active)
{
    __wt_qsort(active->names, active->count, sizeof(char *), active_files_sort_function);
}

/*
 * active_files_remove_missing --
 *     Files in the previous list that are missing from the current list are removed.
 */
static void
active_files_remove_missing(ACTIVE_FILES *prev, ACTIVE_FILES *cur, const char *dirname)
{
    uint32_t curpos, prevpos;
    int cmp;
    char filename[1024];

    active_files_print(prev, "computing removals: previous list of active files");
    active_files_print(cur, "computing removals: current list of active files");
    curpos = 0;
    /*
     * Walk through the two lists looking for non-matches.
     */
    for (prevpos = 0; prevpos < prev->count; prevpos++) {
again:
        if (curpos >= cur->count)
            cmp = -1; /* There are extra entries at the end of the prev list */
        else
            cmp = strcmp(prev->names[prevpos], cur->names[curpos]);

        if (cmp == 0)
            curpos++;
        else if (cmp < 0) {
            /*
             * There is something in the prev list not in the current list. Remove it, and continue
             * - don't advance the current list.
             */
            testutil_check(
              __wt_snprintf(filename, sizeof(filename), "%s/%s", dirname, prev->names[prevpos]));
            VERBOSE(3, "Removing file from backup: %s\n", filename);
            testutil_check(remove(filename));
        } else {
            /*
             * There is something in the current list not in the prev list. Walk past it in the
             * current list and try again.
             */
            curpos++;
            goto again;
        }
    }
}

/*
 * active_files_free --
 *     Free the list of active files.
 */
static void
active_files_free(ACTIVE_FILES *active)
{
    uint32_t i;

    for (i = 0; i < active->count; i++)
        free(active->names[i]);
    free(active->names);
    active_files_init(active);
}

/*
 * active_files_move --
 *     Move an active file list to the destination list.
 */
static void
active_files_move(ACTIVE_FILES *dest, ACTIVE_FILES *src)
{
    active_files_free(dest);
    *dest = *src;
    WT_CLEAR(*src);
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
        nrecords = __wt_random(&table->rand) % 1000;
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
    testutil_check(
      __wt_snprintf(uri, URI_MAX_LEN, URI_FORMAT, (int)slot, (int)tinfo->table[slot].name_index++));

    /*
     * A quarter of the time use a non-default allocation size on the table. This is set
     * independently of the granularity to stress mismatched values.
     */
    if (__wt_random(rand) % 4 == 0) {
        alloc = __wt_random(rand) % NUM_ALLOC;
        allocstr = alloc_sizes[alloc];
        testutil_check(__wt_snprintf(buf, sizeof(buf),
          "%s,allocation_size=%s,internal_page_max=%s,leaf_page_max=%s", TABLE_FORMAT, allocstr,
          allocstr, allocstr));
    } else
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s", TABLE_FORMAT));
    VERBOSE(3, "create %s: %s\n", uri, buf);
    testutil_check(session->create(session, uri, buf));
    tinfo->table[slot].name = uri;
    tinfo->tables_in_use++;
}

/*
 * rename_table --
 *     TODO: Add a comment describing this function.
 */
static void
rename_table(WT_SESSION *session, TABLE_INFO *tinfo, uint32_t slot)
{
    char *olduri, *uri;

    testutil_assert(TABLE_VALID(&tinfo->table[slot]));
    uri = dcalloc(1, URI_MAX_LEN);
    testutil_check(
      __wt_snprintf(uri, URI_MAX_LEN, URI_FORMAT, (int)slot, (int)tinfo->table[slot].name_index++));

    olduri = tinfo->table[slot].name;
    VERBOSE(3, "rename %s %s\n", olduri, uri);
    WT_OP_CHECKPOINT_WAIT(session, session->rename(session, olduri, uri, NULL));
    free(olduri);
    tinfo->table[slot].name = uri;
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
base_backup(WT_CONNECTION *conn, WT_RAND_STATE *rand, const char *home, const char *backup_home,
  TABLE_INFO *tinfo, ACTIVE_FILES *active)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    uint32_t granularity;
    int nfiles, ret;
    char buf[4096];
    char *filename;
    char granularity_unit;
    const char *cons;

    nfiles = 0;

    VERBOSE(2, "BASE BACKUP: %s\n", backup_home);
    active_files_free(active);
    active_files_init(active);
    testutil_check(
      __wt_snprintf(buf, sizeof(buf), "rm -rf %s && mkdir %s", backup_home, backup_home));
    VERBOSE(3, " => %s\n", buf);
    testutil_check(system(buf));

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    tinfo->full_backup_number = tinfo->incr_backup_number++;

    /* Half of the runs with very low granularity to stress bitmaps */
    granularity = __wt_random(rand) % 20;
    if (__wt_random(rand) % 2 == 0) {
        granularity_unit = 'K';
        granularity += 4;
    } else {
        granularity_unit = 'M';
        granularity += 1;
    }
    if (__wt_random(rand) % 2 == 0)
        cons = ",consolidate=true";
    else
        cons = ",consolidate=false";
    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "incremental=(granularity=%" PRIu32 "%c,enabled=true,%s,this_id=ID%" PRIu32 ")", granularity,
      granularity_unit, cons, tinfo->full_backup_number));
    VERBOSE(3, "open_cursor(session, \"backup:\", NULL, \"%s\", &cursor)\n", buf);
    testutil_check(session->open_cursor(session, "backup:", NULL, buf, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        nfiles++;
        testutil_check(cursor->get_key(cursor, &filename));
        active_files_add(active, filename);
        testutil_check(
          __wt_snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, backup_home, filename));
        VERBOSE(3, " => %s\n", buf);
        testutil_check(system(buf));
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    active_files_sort(active);
    VERBOSE(2, " finished base backup: %d files\n", nfiles);
}

/*
 * reopen_file --
 *     Open a file if it isn't already open. The "memory" of the open file name is kept in the
 *     buffer passed in.
 */
static void
reopen_file(int *fdp, char *buf, size_t buflen, const char *filename, int oflag)
{
    /* Do we already have this file open? */
    if (strcmp(buf, filename) == 0 && *fdp != -1)
        return;
    if (*fdp != -1)
        testutil_check(close(*fdp));
    *fdp = open(filename, oflag, 0666);
    strncpy(buf, filename, buflen);
    testutil_assert(*fdp >= 0);
}

/*
 * incr_backup --
 *     Perform an incremental backup into an existing backup directory.
 */
static void
incr_backup(WT_CONNECTION *conn, const char *home, const char *backup_home, TABLE_INFO *tinfo,
  ACTIVE_FILES *current_active)
{
    ACTIVE_FILES active;
    WT_CURSOR *cursor, *file_cursor;
    WT_SESSION *session;
    void *tmp;
    ssize_t rdsize;
    uint64_t offset, size, type;
    int rfd, ret, wfd, nfiles, nrange, ncopy;
    char buf[4096], rbuf[4096], wbuf[4096];
    char *filename;

    VERBOSE(2, "INCREMENTAL BACKUP: %s\n", backup_home);
    active_files_print(current_active, "current list before incremental backup");
    WT_CLEAR(rbuf);
    WT_CLEAR(wbuf);
    rfd = wfd = -1;
    nfiles = nrange = ncopy = 0;

    active_files_init(&active);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(
      __wt_snprintf(buf, sizeof(buf), "incremental=(src_id=ID%" PRIu32 ",this_id=ID%" PRIu32 ")",
        tinfo->full_backup_number, tinfo->incr_backup_number++));
    VERBOSE(3, "open_cursor(session, \"backup:\", NULL, \"%s\", &cursor)\n", buf);
    testutil_check(session->open_cursor(session, "backup:", NULL, buf, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        nfiles++;
        testutil_check(cursor->get_key(cursor, &filename));
        active_files_add(&active, filename);
        if (slow_incremental) {
            /*
             * The "slow" version of an incremental backup is to copy the entire file that was
             * indicated to be changed. This may be useful for debugging problems that occur in
             * backup. This path is typically disabled for the test program.
             */
            testutil_check(__wt_snprintf(
              buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, backup_home, filename));
            VERBOSE(3, " => %s\n", buf);
            testutil_check(system(buf));
        } else {
            /*
             * Here is the normal incremental backup. Now that we know what file has changed, we get
             * the specific changes
             */
            testutil_check(__wt_snprintf(buf, sizeof(buf), "incremental=(file=%s)", filename));
            testutil_check(session->open_cursor(session, NULL, cursor, buf, &file_cursor));
            VERBOSE(3, "open_cursor(session, NULL, cursor, \"%s\", &file_cursor)\n", buf);
            while ((ret = file_cursor->next(file_cursor)) == 0) {
                testutil_check(file_cursor->get_key(file_cursor, &offset, &size, &type));
                testutil_assert(type == WT_BACKUP_FILE || type == WT_BACKUP_RANGE);
                if (type == WT_BACKUP_RANGE) {
                    nrange++;
                    tmp = dcalloc(1, size);

                    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", home, filename));
                    VERBOSE(5, "Reopen read file: %s\n", buf);
                    reopen_file(&rfd, rbuf, sizeof(rbuf), buf, O_RDONLY);
                    rdsize = pread(rfd, tmp, (size_t)size, (wt_off_t)offset);
                    testutil_assert(rdsize >= 0);

                    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", backup_home, filename));
                    VERBOSE(5, "Reopen write file: %s\n", buf);
                    reopen_file(&wfd, wbuf, sizeof(wbuf), buf, O_WRONLY | O_CREAT);
                    /* Use the read size since we may have read less than the granularity. */
                    testutil_assert(pwrite(wfd, tmp, (size_t)rdsize, (wt_off_t)offset) == rdsize);
                    free(tmp);
                } else {
                    ncopy++;
                    testutil_check(__wt_snprintf(
                      buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, backup_home, filename));
                    VERBOSE(3, " => %s\n", buf);
                    testutil_check(system(buf));
                }
            }
            testutil_assert(ret == WT_NOTFOUND);
            testutil_check(file_cursor->close(file_cursor));
        }
    }
    testutil_assert(ret == WT_NOTFOUND);
    if (rfd != -1)
        testutil_check(close(rfd));
    if (wfd != -1)
        testutil_check(close(wfd));
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    VERBOSE(2, " finished incremental backup: %d files, %d range copy, %d file copy\n", nfiles,
      nrange, ncopy);
    active_files_sort(&active);
    active_files_remove_missing(current_active, &active, backup_home);

    /* Move the active list to the current list. */
    active_files_move(current_active, &active);
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
            change_count += 20000;
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
check_backup(const char *backup_home, const char *backup_check, TABLE_INFO *tinfo)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    uint32_t slot;
    char buf[4096];

    VERBOSE(
      2, "CHECK BACKUP: copy %s to %s, then check %s\n", backup_home, backup_check, backup_check);

    testutil_check(__wt_snprintf(
      buf, sizeof(buf), "rm -rf %s && cp -r %s %s", backup_check, backup_home, backup_check));
    testutil_check(system(buf));

    testutil_check(wiredtiger_open(backup_check, NULL, CONN_CONFIG_COMMON, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    for (slot = 0; slot < tinfo->table_count; slot++) {
        if (TABLE_VALID(&tinfo->table[slot]))
            check_table(session, &tinfo->table[slot]);
    }

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    ACTIVE_FILES active;
    TABLE_INFO tinfo;
    WT_CONNECTION *conn;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    uint32_t file_max, iter, max_value_size, next_checkpoint, rough_size, slot;
    int ch, ncheckpoints, nreopens, status;
    const char *backup_verbose, *working_dir;
    char conf[1024], home[1024], backup_check[1024], backup_dir[1024], command[4096];
    bool preserve;

    preserve = false;
    ncheckpoints = nreopens = 0;
    (void)testutil_set_progname(argv);
    custom_die = die; /* Set our own abort handler */
    WT_CLEAR(tinfo);
    active_files_init(&active);

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
    testutil_check(__wt_snprintf(backup_dir, sizeof(backup_dir), "../%s.BACKUP", home));
    testutil_check(__wt_snprintf(backup_check, sizeof(backup_check), "../%s.CHECK", home));
    printf("Seed: %" PRIu64 "\n", seed);

    testutil_check(
      __wt_snprintf(command, sizeof(command), "rm -rf %s %s; mkdir %s", home, backup_dir, home));
    if ((status = system(command)) < 0)
        testutil_die(status, "system: %s", command);

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
        file_max = 200 + __wt_random(&rnd) % 1000; /* 200K to ~1M */
    else
        file_max = 1000 + __wt_random(&rnd) % 20000; /* 1M to ~20M */
    testutil_check(
      __wt_snprintf(conf, sizeof(conf), "%s,create,%s,log=(enabled=true,file_max=%" PRIu32 "K)",
        CONN_CONFIG_COMMON, backup_verbose, file_max));
    VERBOSE(2, "wiredtiger config: %s\n", conf);
    testutil_check(wiredtiger_open(home, NULL, conf, &conn));
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
                 * For schema events, we choose to create, rename or drop tables. We pick a random
                 * slot, and if it is empty, create a table there. Otherwise, we rename or drop.
                 * That should give us a steady state with slots mostly filled.
                 */
                slot = __wt_random(&rnd) % tinfo.table_count;
                if (!TABLE_VALID(&tinfo.table[slot]))
                    create_table(session, &rnd, &tinfo, slot);
                else if (__wt_random(&rnd) % 3 == 0 && do_rename)
                    rename_table(session, &tinfo, slot);
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
        if (__wt_random(&rnd) % 10 == 0) {
            VERBOSE(2, "Close and reopen the connection %d\n", nreopens);
            testutil_check(conn->close(conn, NULL));
            testutil_check(wiredtiger_open(home, NULL, conf, &conn));
            testutil_check(conn->open_session(conn, NULL, NULL, &session));
            nreopens++;
        }

        if (iter == 0) {
            base_backup(conn, &rnd, home, backup_dir, &tinfo, &active);
            check_backup(backup_dir, backup_check, &tinfo);
        } else {
            incr_backup(conn, home, backup_dir, &tinfo, &active);
            check_backup(backup_dir, backup_check, &tinfo);
            if (__wt_random(&rnd) % 10 == 0) {
                base_backup(conn, &rnd, home, backup_dir, &tinfo, &active);
                check_backup(backup_dir, backup_check, &tinfo);
            }
        }
    }
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
    active_files_free(&active);
    tables_free(&tinfo);

    printf("Success.\n");
    if (!preserve) {
        testutil_clean_test_artifacts(home);
        testutil_clean_work_dir(home);
    }

    return (EXIT_SUCCESS);
}
