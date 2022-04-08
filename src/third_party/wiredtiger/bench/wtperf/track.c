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

#include "wtperf.h"

/*
 * Return total insert operations for the populate phase.
 */
uint64_t
sum_pop_ops(WTPERF *wtperf)
{
    CONFIG_OPTS *opts;
    WTPERF_THREAD *thread;
    uint64_t total;
    u_int i;

    opts = wtperf->opts;
    total = 0;

    for (i = 0, thread = wtperf->popthreads; thread != NULL && i < opts->populate_threads;
         ++i, ++thread)
        total += thread->insert.ops;
    return (total);
}

/*
 * Return total backup operations.
 */
uint64_t
sum_backup_ops(WTPERF *wtperf)
{
    CONFIG_OPTS *opts;
    uint64_t total;

    opts = wtperf->opts;

    if (opts->backup_interval > 0)
        total = wtperf->backupthreads->backup.ops;
    else
        total = 0;
    return (total);
}

/*
 * Return total checkpoint operations.
 */
uint64_t
sum_ckpt_ops(WTPERF *wtperf)
{
    CONFIG_OPTS *opts;
    WTPERF_THREAD *thread;
    uint64_t total;
    u_int i;

    opts = wtperf->opts;
    total = 0;

    for (i = 0, thread = wtperf->ckptthreads; thread != NULL && i < opts->checkpoint_threads;
         ++i, ++thread)
        total += thread->ckpt.ops;
    return (total);
}

/*
 * Return total flush_tier operations.
 */
uint64_t
sum_flush_ops(WTPERF *wtperf)
{
    CONFIG_OPTS *opts;
    uint64_t total;

    opts = wtperf->opts;

    if (opts->tiered_flush_interval > 0)
        total = wtperf->flushthreads->flush.ops;
    else
        total = 0;
    return (total);
}

/*
 * Return total scan operations.
 */
uint64_t
sum_scan_ops(WTPERF *wtperf)
{
    CONFIG_OPTS *opts;
    uint64_t total;

    opts = wtperf->opts;

    if (opts->scan_interval > 0)
        total = wtperf->scanthreads->scan.ops;
    else
        total = 0;
    return (total);
}

/*
 * Return total operations count for the worker threads.
 */
static uint64_t
sum_ops(WTPERF *wtperf, size_t field_offset)
{
    CONFIG_OPTS *opts;
    WTPERF_THREAD *thread;
    uint64_t total;
    int64_t i, th_cnt;

    opts = wtperf->opts;
    total = 0;

    if (wtperf->popthreads == NULL) {
        thread = wtperf->workers;
        th_cnt = wtperf->workers_cnt;
    } else {
        thread = wtperf->popthreads;
        th_cnt = opts->populate_threads;
    }
    for (i = 0; thread != NULL && i < th_cnt; ++i, ++thread)
        total += ((TRACK *)((uint8_t *)thread + field_offset))->ops;

    return (total);
}
uint64_t
sum_insert_ops(WTPERF *wtperf)
{
    return (sum_ops(wtperf, offsetof(WTPERF_THREAD, insert)));
}
uint64_t
sum_modify_ops(WTPERF *wtperf)
{
    return (sum_ops(wtperf, offsetof(WTPERF_THREAD, modify)));
}
uint64_t
sum_read_ops(WTPERF *wtperf)
{
    return (sum_ops(wtperf, offsetof(WTPERF_THREAD, read)));
}
uint64_t
sum_truncate_ops(WTPERF *wtperf)
{
    return (sum_ops(wtperf, offsetof(WTPERF_THREAD, truncate)));
}
uint64_t
sum_update_ops(WTPERF *wtperf)
{
    return (sum_ops(wtperf, offsetof(WTPERF_THREAD, update)));
}

/*
 * latency_op --
 *     Get average, minimum and maximum latency for this period for a particular operation.
 */
static void
latency_op(WTPERF *wtperf, size_t field_offset, uint32_t *avgp, uint32_t *minp, uint32_t *maxp)
{
    CONFIG_OPTS *opts;
    TRACK *track;
    WTPERF_THREAD *thread;
    uint64_t ops, latency, tmp;
    int64_t i, th_cnt;
    uint32_t max, min;

    opts = wtperf->opts;
    ops = latency = 0;
    max = 0;
    min = UINT32_MAX;

    if (wtperf->popthreads == NULL) {
        thread = wtperf->workers;
        th_cnt = wtperf->workers_cnt;
    } else {
        thread = wtperf->popthreads;
        th_cnt = opts->populate_threads;
    }
    for (i = 0; thread != NULL && i < th_cnt; ++i, ++thread) {
        track = (TRACK *)((uint8_t *)thread + field_offset);
        tmp = track->latency_ops;
        ops += tmp - track->last_latency_ops;
        track->last_latency_ops = tmp;
        tmp = track->latency;
        latency += tmp - track->last_latency;
        track->last_latency = tmp;

        if (min > track->min_latency)
            min = track->min_latency;
        track->min_latency = UINT32_MAX;
        if (max < track->max_latency)
            max = track->max_latency;
        track->max_latency = 0;
    }

    if (ops == 0)
        *avgp = *minp = *maxp = 0;
    else {
        *minp = min;
        *maxp = max;
        *avgp = (uint32_t)(latency / ops);
    }
}
void
latency_insert(WTPERF *wtperf, uint32_t *avgp, uint32_t *minp, uint32_t *maxp)
{
    static uint32_t last_avg = 0, last_max = 0, last_min = 0;

    latency_op(wtperf, offsetof(WTPERF_THREAD, insert), avgp, minp, maxp);

    /*
     * If nothing happened, graph the average, minimum and maximum as they were the last time, it
     * keeps the graphs from having discontinuities.
     */
    if (*minp == 0) {
        *avgp = last_avg;
        *minp = last_min;
        *maxp = last_max;
    } else {
        last_avg = *avgp;
        last_min = *minp;
        last_max = *maxp;
    }
}
void
latency_modify(WTPERF *wtperf, uint32_t *avgp, uint32_t *minp, uint32_t *maxp)
{
    static uint32_t last_avg = 0, last_max = 0, last_min = 0;

    latency_op(wtperf, offsetof(WTPERF_THREAD, modify), avgp, minp, maxp);

    /*
     * If nothing happened, graph the average, minimum and maximum as they were the last time, it
     * keeps the graphs from having discontinuities.
     */
    if (*minp == 0) {
        *avgp = last_avg;
        *minp = last_min;
        *maxp = last_max;
    } else {
        last_avg = *avgp;
        last_min = *minp;
        last_max = *maxp;
    }
}
void
latency_read(WTPERF *wtperf, uint32_t *avgp, uint32_t *minp, uint32_t *maxp)
{
    static uint32_t last_avg = 0, last_max = 0, last_min = 0;

    latency_op(wtperf, offsetof(WTPERF_THREAD, read), avgp, minp, maxp);

    /*
     * If nothing happened, graph the average, minimum and maximum as they were the last time, it
     * keeps the graphs from having discontinuities.
     */
    if (*minp == 0) {
        *avgp = last_avg;
        *minp = last_min;
        *maxp = last_max;
    } else {
        last_avg = *avgp;
        last_min = *minp;
        last_max = *maxp;
    }
}
void
latency_update(WTPERF *wtperf, uint32_t *avgp, uint32_t *minp, uint32_t *maxp)
{
    static uint32_t last_avg = 0, last_max = 0, last_min = 0;

    latency_op(wtperf, offsetof(WTPERF_THREAD, update), avgp, minp, maxp);

    /*
     * If nothing happened, graph the average, minimum and maximum as they were the last time, it
     * keeps the graphs from having discontinuities.
     */
    if (*minp == 0) {
        *avgp = last_avg;
        *minp = last_min;
        *maxp = last_max;
    } else {
        last_avg = *avgp;
        last_min = *minp;
        last_max = *maxp;
    }
}

/*
 * sum_latency --
 *     Sum latency for a set of threads.
 */
static void
sum_latency(WTPERF *wtperf, size_t field_offset, TRACK *total)
{
    WTPERF_THREAD *thread;
    TRACK *trk;
    int64_t i;
    u_int j;

    memset(total, 0, sizeof(*total));

    for (i = 0, thread = wtperf->workers; thread != NULL && i < wtperf->workers_cnt;
         ++i, ++thread) {
        trk = (TRACK *)((uint8_t *)thread + field_offset);

        for (j = 0; j < ELEMENTS(trk->us); ++j) {
            total->ops += trk->us[j];
            total->us[j] += trk->us[j];
        }
        for (j = 0; j < ELEMENTS(trk->ms); ++j) {
            total->ops += trk->ms[j];
            total->ms[j] += trk->ms[j];
        }
        for (j = 0; j < ELEMENTS(trk->sec); ++j) {
            total->ops += trk->sec[j];
            total->sec[j] += trk->sec[j];
        }
    }
}
static void
sum_insert_latency(WTPERF *wtperf, TRACK *total)
{
    sum_latency(wtperf, offsetof(WTPERF_THREAD, insert), total);
}
static void
sum_modify_latency(WTPERF *wtperf, TRACK *total)
{
    sum_latency(wtperf, offsetof(WTPERF_THREAD, modify), total);
}
static void
sum_read_latency(WTPERF *wtperf, TRACK *total)
{
    sum_latency(wtperf, offsetof(WTPERF_THREAD, read), total);
}
static void
sum_update_latency(WTPERF *wtperf, TRACK *total)
{
    sum_latency(wtperf, offsetof(WTPERF_THREAD, update), total);
}

static void
latency_print_single(WTPERF *wtperf, TRACK *total, const char *name)
{
    FILE *fp;
    u_int i;
    uint64_t cumops;
    char path[1024];

    testutil_check(__wt_snprintf(path, sizeof(path), "%s/latency.%s", wtperf->monitor_dir, name));
    if ((fp = fopen(path, "w")) == NULL) {
        lprintf(wtperf, errno, 0, "%s", path);
        return;
    }

    fprintf(fp, "#usecs,operations,cumulative-operations,total-operations\n");
    cumops = 0;
    for (i = 0; i < ELEMENTS(total->us); ++i) {
        if (total->us[i] == 0)
            continue;
        cumops += total->us[i];
        fprintf(fp, "%u,%" PRIu32 ",%" PRIu64 ",%" PRIu64 "\n", (i + 1), total->us[i], cumops,
          total->ops);
    }
    for (i = 1; i < ELEMENTS(total->ms); ++i) {
        if (total->ms[i] == 0)
            continue;
        cumops += total->ms[i];
        fprintf(fp, "%llu,%" PRIu32 ",%" PRIu64 ",%" PRIu64 "\n", ms_to_us(i + 1), total->ms[i],
          cumops, total->ops);
    }
    for (i = 1; i < ELEMENTS(total->sec); ++i) {
        if (total->sec[i] == 0)
            continue;
        cumops += total->sec[i];
        fprintf(fp, "%llu,%" PRIu32 ",%" PRIu64 ",%" PRIu64 "\n", sec_to_us(i + 1), total->sec[i],
          cumops, total->ops);
    }

    (void)fclose(fp);
}

void
latency_print(WTPERF *wtperf)
{
    TRACK total;

    sum_insert_latency(wtperf, &total);
    latency_print_single(wtperf, &total, "insert");
    sum_modify_latency(wtperf, &total);
    latency_print_single(wtperf, &total, "modify");
    sum_read_latency(wtperf, &total);
    latency_print_single(wtperf, &total, "read");
    sum_update_latency(wtperf, &total);
    latency_print_single(wtperf, &total, "update");
}
