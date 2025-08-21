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
#include <queue.h>
#include "test_util.h"
#include "bench_timer.h"

/* These are the test_util options that we want: preserve, number of threads and home directory. */
#define SHARED_PARSE_OPTIONS "pT:h:"

/*
 * This workload is dynamic.  It creates tables in batches periodically, a proportion of these
 * ("table_in_use_percent") are "kept alive" by simple search or update calls.
 * As the workload continues, more and more tables are created until we reach "table_count".
 * Then, as we create more, we drop an equal amount.
 */
#define TABLES_PER_WORK_ITEM 10
#define SECONDS_BETWEEN_WORK_QUEUE_REFILL 2
#define SECONDS_BETWEEN_CHECKPOINTS 20

typedef struct work_item {
    int tablenums[TABLES_PER_WORK_ITEM];
    TAILQ_ENTRY(work_item) q;
} WORK_ITEM;

typedef struct {
    TEST_OPTS opts;
    struct {
        int creation_time_percent;
        int table_count;
        int run_time;
        int seconds_until_full;
        int table_in_use_percent;
        int tables_per_work_item;
    } config;

    WT_CONNECTION *conn;

    bool done;
    bool started;
    bool checkpointing;

    int checkpoint_num;

    int high;   /* high point for active tables */
    int low;    /* low point for active tables */
    int exists; /* tables under this number have been dropped */

    /* Locked */
    TAILQ_HEAD(work_queue, work_item) work_queue;
    int work_queue_len;
    pthread_rwlock_t work_queue_lock;
} SHARED;

typedef struct {
    SHARED *shared;
    int threadnum;

    BENCH_TIMER t_create;
    BENCH_TIMER t_checkpoint;
    BENCH_TIMER t_drop;
    BENCH_TIMER t_first_insert;
    BENCH_TIMER t_read;
    BENCH_TIMER t_update;
} THREAD_ARGS;

/* Constants and variables declaration. */
static const char conn_config[] =
  "create,cache_size=2GB,statistics=(all),statistics_log=(json,on_close,wait=1)";
static const char table_config[] = "leaf_page_max=64KB,key_format=i,value_format=i";

extern char *__wt_optarg;

/* Forward declarations. */
static void bench_dhandle(SHARED *);
static void bench_dhandle_run(SHARED *);
static void *checkpointer(void *);
static void *creator(void *);
static void *queuer(void *);
static void shuffle(int *arr, int n, WT_RAND_STATE *rnd);
static void *worker(void *);

#define WITH_RW_LOCK(lock, e)              \
    do {                                   \
        (void)pthread_rwlock_wrlock(lock); \
        e;                                 \
        (void)pthread_rwlock_unlock(lock); \
    } while (0)

/*
 * usage --
 *     Display usage statement and exit failure.
 */
static int
usage(void)
{
    fprintf(stderr,
      "usage: %s\n"
      "    [-p] [-c create_pct] [ -f full_seconds ] [ -n table_count ]\n"
      "    [ -r run_time ] [-h home] [-T threads] [ -u in_use_percent ] [ -w tables_per_work_item "
      "]\n",
      progname);
    fprintf(stderr, "%s",
      "\t-h set a database home directory\n"
      "\t-c percentage of time doing create calls\n"
      "\t-f seconds until reaching the table count (this is a goal, the performance may not allow "
      "it)\n"
      "\t-n table count\n"
      "\t-p preserve home directory\n"
      "\t-r total run time\n"
      "\t-T set number of threads\n"
      "\t-u percentage of tables kept in use\n"
      "\t-w tables accessed per work item\n");
    return (EXIT_FAILURE);
}

/*
 * main --
 *     Parse options, run workload and clean up.
 */
int
main(int argc, char *argv[])
{
    SHARED shared;
    int ch;

    memset(&shared, 0, sizeof(shared));

    __wt_stream_set_line_buffer(stdout);

    (void)testutil_set_progname(argv);
    testutil_parse_begin_opt(argc, argv, SHARED_PARSE_OPTIONS, &shared.opts);

    while ((ch = __wt_getopt(progname, argc, argv, "c:f:n:r:u:w:" SHARED_PARSE_OPTIONS)) != EOF)
        switch (ch) {
        case 'c':
            shared.config.creation_time_percent = atoi(__wt_optarg);
            break;

        case 'f':
            shared.config.seconds_until_full = atoi(__wt_optarg);
            break;

        case 'n':
            shared.config.table_count = atoi(__wt_optarg);
            break;

        case 'r':
            shared.config.run_time = atoi(__wt_optarg);
            break;

        case 'u':
            shared.config.table_in_use_percent = atoi(__wt_optarg);
            break;

        case 'w':
            shared.config.tables_per_work_item = atoi(__wt_optarg);
            break;

        default:
            /* The option is either one that we're asking testutil to support, or illegal. */
            if (testutil_parse_single_opt(&shared.opts, ch) != 0)
                return (usage());
        }
    testutil_parse_end_opt(&shared.opts);

    /* Set defaults. */
    if (shared.opts.nthreads == 0)
        shared.opts.nthreads = 10;
    if (shared.config.creation_time_percent == 0)
        shared.config.creation_time_percent = 50;
    if (shared.config.seconds_until_full == 0)
        shared.config.seconds_until_full = 300;
    if (shared.config.run_time == 0)
        shared.config.run_time = 2400;
    if (shared.config.table_count == 0)
        shared.config.table_count = 50000;
    if (shared.config.table_in_use_percent == 0)
        shared.config.table_in_use_percent = 75;
    if (shared.config.tables_per_work_item == 0)
        shared.config.tables_per_work_item = 10;

    bench_dhandle(&shared);

    testutil_cleanup(&shared.opts);

    return (EXIT_SUCCESS);
}

/*
 * bench_dhandle --
 *     Set up and initialization to do the benchmark run.
 */
static void
bench_dhandle(SHARED *shared)
{
    char *home = shared->opts.home;

    testutil_recreate_dir(home);
    testutil_wiredtiger_open(&shared->opts, home, conn_config, NULL, &shared->conn, false, true);

    bench_dhandle_run(shared);

    /* Cleanup */
    if (!shared->opts.preserve)
        testutil_remove(home);
}

/*
 * bench_dhandle_run --
 *     Run the benchmark.
 */
static void
bench_dhandle_run(SHARED *shared)
{
    THREAD_ARGS *args;
    pthread_t *tid;
    uint64_t i, now, done_time, last_minute, start_time, stop_time;
    void *ignored;
    int checkpoint_num, last_checkpoint_num;
    BENCH_TIMER t_last_checkpoint, t_last_create, t_last_drop, t_last_first_insert, t_last_read,
      t_last_update;
    BENCH_TIMER t_minute_checkpoint, t_minute_create, t_minute_drop, t_minute_first_insert,
      t_minute_read, t_minute_update;
    void *(*thread_func)(void *);

    /* extra threads for create, queue, checkpoint */
    args = dcalloc(shared->opts.nthreads + 3, sizeof(THREAD_ARGS));
    tid = dcalloc(shared->opts.nthreads + 3, sizeof(pthread_t));

    /* timers that keep the perf numbers from the last time. */
    bench_timer_init(&t_last_checkpoint, NULL);
    bench_timer_init(&t_last_create, NULL);
    bench_timer_init(&t_last_drop, NULL);
    bench_timer_init(&t_last_first_insert, NULL);
    bench_timer_init(&t_last_read, NULL);
    bench_timer_init(&t_last_update, NULL);

    /* timers that keep the perf numbers for the previous minute. */
    bench_timer_init(&t_minute_checkpoint, NULL);
    bench_timer_init(&t_minute_create, NULL);
    bench_timer_init(&t_minute_drop, NULL);
    bench_timer_init(&t_minute_first_insert, NULL);
    bench_timer_init(&t_minute_read, NULL);
    bench_timer_init(&t_minute_update, NULL);

    memset(args, 0, sizeof(THREAD_ARGS) * (shared->opts.nthreads + 3));
    last_checkpoint_num = 0;

    printf("Running with %" PRIu64
           " threads, %d tables, %d seconds until full, %d seconds total, "
           "%d%% tables in use, %d tables per work item\n",
      shared->opts.nthreads, shared->config.table_count, shared->config.seconds_until_full,
      shared->config.run_time, shared->config.table_in_use_percent,
      shared->config.tables_per_work_item);

    pthread_rwlock_init(&shared->work_queue_lock, NULL);

    for (i = 0; i < shared->opts.nthreads + 3; ++i) {
        args[i].threadnum = (int)i;
        args[i].shared = shared;

        bench_timer_init(&args[i].t_create, NULL);
        bench_timer_init(&args[i].t_checkpoint, NULL);
        bench_timer_init(&args[i].t_drop, NULL);
        bench_timer_init(&args[i].t_first_insert, NULL);
        bench_timer_init(&args[i].t_read, NULL);
        bench_timer_init(&args[i].t_update, NULL);

        if (i == shared->opts.nthreads)
            thread_func = creator;
        else if (i == shared->opts.nthreads + 1)
            thread_func = queuer;
        else if (i == shared->opts.nthreads + 2)
            thread_func = checkpointer;
        else
            thread_func = worker;

        testutil_check(pthread_create(&tid[i], NULL, thread_func, &args[i]));
    }

    __wt_seconds(NULL, &start_time);
    stop_time = start_time + (uint64_t)shared->config.run_time;
    done_time = 0;
    last_minute = start_time;
    do {
        BENCH_TIMER t_checkpoint, t_create, t_drop, t_first_insert, t_read, t_update;
        bool printed = false;

        bench_timer_init(&t_checkpoint, "checkpoint");
        bench_timer_init(&t_create, "create");
        bench_timer_init(&t_drop, "drop");
        bench_timer_init(&t_first_insert, "first_insert");
        bench_timer_init(&t_read, "read");
        bench_timer_init(&t_update, "update");

        sleep(5);
        for (i = 0; i < shared->opts.nthreads + 3; ++i) {
            bench_timer_add_from_shared(&t_checkpoint, &args[i].t_checkpoint);
            bench_timer_add_from_shared(&t_create, &args[i].t_create);
            bench_timer_add_from_shared(&t_drop, &args[i].t_drop);
            bench_timer_add_from_shared(&t_first_insert, &args[i].t_first_insert);
            bench_timer_add_from_shared(&t_read, &args[i].t_read);
            bench_timer_add_from_shared(&t_update, &args[i].t_update);
        }
        __wt_seconds(NULL, &now);
        int active = shared->high - shared->low;
        printf("\n%" PRIu64 " seconds, %d active tables", (now - start_time), active);
        WT_ACQUIRE_READ_WITH_BARRIER(checkpoint_num, shared->checkpoint_num);
        if (last_checkpoint_num != checkpoint_num || shared->checkpointing) {
            printf(" ** %s number %d **",
              shared->checkpointing ? "checkpointing" : "finished checkpoint", checkpoint_num);
            last_checkpoint_num = checkpoint_num;
        }
        printf("\n");

#define TIMER_SHOW(prev_timer, now_timer)                     \
    do {                                                      \
        if (bench_timer_show_change(&prev_timer, &now_timer)) \
            printed = true;                                   \
        prev_timer = now_timer;                               \
    } while (0)

        TIMER_SHOW(t_last_checkpoint, t_checkpoint);
        TIMER_SHOW(t_last_create, t_create);
        TIMER_SHOW(t_last_drop, t_drop);
        TIMER_SHOW(t_last_first_insert, t_first_insert);
        TIMER_SHOW(t_last_read, t_read);
        TIMER_SHOW(t_last_update, t_update);

        if (!printed)
            printf("(no reads or updates)");
        printf("\n");

        if (now >= last_minute + 60) {
            printed = false;
            printf("\n**** PREVIOUS MINUTE AGGREGATED ****\n");
            TIMER_SHOW(t_minute_checkpoint, t_checkpoint);
            TIMER_SHOW(t_minute_create, t_create);
            TIMER_SHOW(t_minute_drop, t_drop);
            TIMER_SHOW(t_minute_first_insert, t_first_insert);
            TIMER_SHOW(t_minute_read, t_read);
            TIMER_SHOW(t_minute_update, t_update);
            if (!printed)
                printf("(no activity)");
            printf("************************************\n\n");

            last_minute = now;
        }
        /*
         * If we've reached the number of tables we need to create, then we can quit the run after 5
         * minutes. Note that the table calculation has a little slop in it due to the way the table
         * counts are created.
         */
        if (active > shared->config.table_count - 10) {
            if (done_time == 0)
                done_time = now;
            if (now > done_time + 300)
                break;
        }
    } while (now < stop_time && !shared->done);

    shared->done = true;

    for (i = 0; i < shared->opts.nthreads + 3; ++i)
        testutil_check(pthread_join(tid[i], &ignored));
}

/*
 * checkpointer --
 *     Run the checkpoint loop in a thread context.
 */
static void *
checkpointer(void *void_args)
{
    WT_SESSION *session;
    THREAD_ARGS *args;
    SHARED *shared;

    args = (THREAD_ARGS *)void_args;
    shared = args->shared;
    testutil_check(shared->conn->open_session(shared->conn, NULL, NULL, &session));
    while (!shared->done) {
        sleep(SECONDS_BETWEEN_CHECKPOINTS);
        WT_RELEASE_WRITE_WITH_BARRIER(shared->checkpoint_num, shared->checkpoint_num + 1);
        WT_RELEASE_WRITE_WITH_BARRIER(shared->checkpointing, true);
        BENCH_TIME_CUMULATIVE(
          &args->t_checkpoint, session, { testutil_check(session->checkpoint(session, NULL)); });
        WT_RELEASE_WRITE_WITH_BARRIER(shared->checkpointing, false);
    }
    return (NULL);
}

/*
 * checkpointer --
 *     Run the creator loop in a thread context.
 */
static void *
creator(void *void_args)
{
    BENCH_TIMER create_time, loop_time;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    THREAD_ARGS *args;
    SHARED *shared;
    double create_pct;
    uint64_t create_nsec, elapsed, loop_nsec, pause_nsec, start_time, now;
    int drop_num, table_num;
    int new_table_count, new_exists, new_high, new_low, pct, target_high;
    char tname[100];

    args = (THREAD_ARGS *)void_args;
    shared = args->shared;
    testutil_check(shared->conn->open_session(shared->conn, NULL, NULL, &session));
    __wt_seconds(NULL, &start_time);

    /*
     * The creator thread runs first - the queuer thread and the worker threads wait until the
     * creator finishes one iteration, so that they have some tables to work with.
     */
    while (!shared->done) {
        /*
         * Create a batch of tables before sleeping. We have a target number of tables to get to,
         * and a target time to do it. Based on the current time, see what our new target high
         * should be before sleeping.
         */
        __wt_seconds(NULL, &now);
        elapsed = now - start_time;
        target_high =
          (int)(((double)shared->config.table_count * elapsed) / shared->config.seconds_until_full);

        printf("current high = %d, low = %d, exists = %d. creating tables in range %d => %d\n",
          shared->high, shared->low, shared->exists, shared->high, target_high);
        for (table_num = shared->high; table_num < target_high && !shared->done; ++table_num) {
            /* Measure the total time of the loop. */
            bench_timer_init(&loop_time, NULL);
            bench_timer_start(&loop_time, session);

            snprintf(tname, sizeof(tname), "table:t%d", table_num);

            /*
             * Measure the time of the individual create before adding it to the total.
             */
            BENCH_TIME_SINGLE(
              &create_time, session, { session->create(session, tname, table_config); });
            bench_timer_add(&args->t_create, &create_time);

            BENCH_TIME_CUMULATIVE(&args->t_first_insert, session, {
                testutil_check(session->open_cursor(session, tname, NULL, NULL, &cursor));
                cursor->set_key(cursor, 0);
                cursor->set_value(cursor, table_num);
                cursor->insert(cursor);
                testutil_check(cursor->close(cursor));
            });

            new_high = table_num + 1;

            /* Given that high point, the lowest table that can exist can be determined. */
            new_exists = 0;
            if (new_high > shared->config.table_count)
                new_exists = new_high - shared->config.table_count;

            /*
             * The low marker for being "in use" can be determined, it is based on a configured
             * percentage of in use.
             */
            new_table_count = new_high - new_exists;
            pct = 100 - shared->config.table_in_use_percent;
            new_low = (int)(new_exists + ((double)pct * new_table_count) / 100);

            /*
              printf("  new high = %d, low = %d, exists = %d\n",
              new_high, new_low, new_exists);
            */

            /*
             * Bump up the low point if need be.
             */
            if (shared->low < new_low)
                WT_RELEASE_WRITE_WITH_BARRIER(shared->low, shared->low + 1);

            /*
             * Drop any tables that should be dropped.
             */
            for (drop_num = shared->exists; drop_num < new_exists && !shared->done; ++drop_num) {
                snprintf(tname, sizeof(tname), "table:t%d", drop_num);

                BENCH_TIME_CUMULATIVE(
                  &args->t_drop, session, { session->drop(session, tname, NULL); });
            }

            /* Publish the new values for the high, low and exists points. */
            WT_RELEASE_WRITE_WITH_BARRIER(shared->high, new_high);
            WT_RELEASE_WRITE_WITH_BARRIER(shared->low, new_low);
            WT_RELEASE_WRITE_WITH_BARRIER(shared->exists, new_exists);

            /*
             * We want to pause a certain amount so that the creation
             * takes up a certain percentage (pct) of the time.  The percentage is given by:
             *     create_pct == ((create_nsec) / (loop_nsec + pause_nsec)) * 100
             * Solving for pause_nsec:
             *     pause_nsec == (create_time_nsec * 100 / create_pct) - loop_time_nsec;
             */
            bench_timer_stop(&loop_time, session);

            create_nsec = create_time.total_nsec;
            loop_nsec = loop_time.total_nsec;
            create_pct = (double)shared->config.creation_time_percent;
            pause_nsec = (uint64_t)(((create_nsec * 100) / create_pct) - loop_nsec);
            usleep((useconds_t)pause_nsec / WT_THOUSAND);
        }

        /* We've done at least one round of creates, signal other threads that they can start. */
        shared->started = true;

        sleep(5);
    }
    return (NULL);
}

/*
 * shuffle --
 *     Implements the Fisher-Yates shuffle.
 */
static void
shuffle(int *arr, int n, WT_RAND_STATE *rnd)
{
    int i, j, r, t;

    if (n > 1)
        for (i = n - 1; i < 0; i--) {
            r = (int)__wt_random(rnd);
            /* Get a random number between 0 and i */
            j = (r % i);

            /* swap i and j */
            t = arr[i];
            arr[i] = arr[j];
            arr[j] = t;
        }
}

/*
 * queuer --
 *     Run the queueing loop in a thread context. The queuer adds items to the work queue that would
 *     access each table indicated by the current high and low marks.
 */
static void *
queuer(void *void_args)
{
    SHARED *shared;
    THREAD_ARGS *args;
    WORK_ITEM *work;
    WT_RAND_STATE rnd;
    int i;
    int *table_numbers;
    int table_per_item, table_count, items_to_add, queue_len, tablenum;
    int idx;

    __wt_random_init_default(&rnd);
    args = (THREAD_ARGS *)void_args;
    shared = args->shared;
    table_numbers = NULL;

    while (!shared->started)
        sleep(1);

    while (!shared->done) {
        /* Detect if the work queue is overly large */
        table_per_item = shared->config.tables_per_work_item;
        table_count = shared->high - shared->low;
        queue_len = shared->work_queue_len;
        items_to_add = (table_count + table_per_item - 1);
        if (queue_len > 100 && queue_len > 5 * items_to_add) {
            fprintf(stderr,
              "work queue too long, workers cannot keep up: len=%d, adding=%d active tables=%d\n",
              queue_len, items_to_add, shared->high - shared->low);

            /* Give up, tell everyone to exit. */
            shared->done = true;
            break;
        }
        /* Create the work list, we want to use each table number once. */
        if (queue_len > 0)
            printf("Work queue length at %d, adding %d items for range %d, %d\n", queue_len,
              items_to_add, shared->low, shared->high);
        table_numbers = drealloc(table_numbers, sizeof(int) * (size_t)table_count);
        for (i = 0; i < table_count; ++i)
            table_numbers[i] = i;
        shuffle(table_numbers, table_count, &rnd);
        for (idx = 0; idx < table_count;) {
            work = dcalloc(1, sizeof(WORK_ITEM));
            for (i = 0; i < TABLES_PER_WORK_ITEM && idx < table_count; ++i) {
                tablenum = table_numbers[idx++];

                /*
                 * Mark some items to be updates.
                 */
                if (__wt_random(&rnd) % 100 >= 90)
                    tablenum = -tablenum;
                work->tablenums[i] = tablenum;
            }
            WITH_RW_LOCK(&shared->work_queue_lock, {
                TAILQ_INSERT_HEAD(&shared->work_queue, work, q);
                shared->work_queue_len++;
            });
        }
        sleep(SECONDS_BETWEEN_WORK_QUEUE_REFILL);
    }
    return (NULL);
}

/*
 * worker --
 *     Run the worker loop in a thread context. A worker (of which there may be many) takes an item
 *     from the work queue, which indicates a set of tables to search or update.
 */
static void *
worker(void *void_args)
{
    WT_SESSION *session;
    WT_CURSOR *cursor;
    THREAD_ARGS *args;
    WORK_ITEM *work_item;
    SHARED *shared;
    uint64_t ignore;
    int i, table_num;
    char tname[100];
    bool update;

    args = (THREAD_ARGS *)void_args;
    shared = args->shared;
    testutil_check(shared->conn->open_session(shared->conn, NULL, NULL, &session));

    while (!shared->started)
        sleep(1);

    while (!shared->done) {
        WITH_RW_LOCK(&shared->work_queue_lock, {
            work_item = TAILQ_FIRST(&shared->work_queue);
            if (work_item != NULL) {
                TAILQ_REMOVE(&shared->work_queue, work_item, q);
                shared->work_queue_len--;
            }
        });
        if (work_item == NULL)
            usleep(100);
        else {
            for (i = 0; i < TABLES_PER_WORK_ITEM && !shared->done; ++i) {
                table_num = work_item->tablenums[i];
                update = (table_num < 0);
                if (table_num < 0)
                    table_num = -table_num;
                snprintf(tname, sizeof(tname), "table:t%d", table_num);

                /* Measure the operation. */
                if (update) {
                    BENCH_TIME_CUMULATIVE(&args->t_update, session, {
                        testutil_check(session->open_cursor(session, tname, NULL, NULL, &cursor));
                        cursor->set_key(cursor, 0);
                        cursor->set_value(cursor, args->threadnum);
                        cursor->insert(cursor);
                        testutil_check(cursor->close(cursor));
                    });
                } else {
                    BENCH_TIME_CUMULATIVE(&args->t_read, session, {
                        testutil_check(session->open_cursor(session, tname, NULL, NULL, &cursor));
                        cursor->set_key(cursor, 0);
                        cursor->search(cursor);
                        cursor->get_value(cursor, &ignore);
                        testutil_check(cursor->close(cursor));
                    });
                }
            }
            free(work_item);
        }
        session->reset(session);
    }
    testutil_check(session->close(session, NULL));
    return (NULL);
}
