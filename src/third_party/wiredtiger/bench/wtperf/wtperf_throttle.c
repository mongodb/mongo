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
 * Put the initial config together for running a throttled workload.
 */
void
setup_throttle(WTPERF_THREAD *thread)
{
    THROTTLE_CONFIG *throttle_cfg;

    throttle_cfg = &thread->throttle_cfg;

    /*
     * Setup how the number of operations to run each interval in order to
     * meet our desired max throughput.
     * - If we have a very small number of them we can do one op
     *   on a larger increment. Given there is overhead in throttle logic
     *   we want to avoid running the throttle check regularly.
     * - For most workloads, we aim to do 100 ops per interval and adjust
     *   the sleep period accordingly.
     * - For high throughput workloads, we aim to do many ops in 100us
     *   increments.
     */

    if (thread->workload->throttle < THROTTLE_OPS) {
        /* If the interval is very small, we do one operation */
        throttle_cfg->usecs_increment = USEC_PER_SEC / thread->workload->throttle;
        throttle_cfg->ops_per_increment = 1;
    } else if (thread->workload->throttle < USEC_PER_SEC / THROTTLE_OPS) {
        throttle_cfg->usecs_increment = USEC_PER_SEC / thread->workload->throttle * THROTTLE_OPS;
        throttle_cfg->ops_per_increment = THROTTLE_OPS;
    } else {
        /* If the interval is large, we do more ops per interval */
        throttle_cfg->usecs_increment = USEC_PER_SEC / THROTTLE_OPS;
        throttle_cfg->ops_per_increment = thread->workload->throttle / THROTTLE_OPS;
    }

    /* Give the queue some initial operations to work with */
    throttle_cfg->ops_count = throttle_cfg->ops_per_increment;

    /* Set the first timestamp of when we incremented */
    __wt_epoch(NULL, &throttle_cfg->last_increment);
}

/*
 * Run the throttle function. We will sleep if needed and then reload the counter to perform more
 * operations.
 */
void
worker_throttle(WTPERF_THREAD *thread)
{
    THROTTLE_CONFIG *throttle_cfg;
    struct timespec now;
    uint64_t usecs_delta;

    throttle_cfg = &thread->throttle_cfg;

    __wt_epoch(NULL, &now);

    /*
     * If we did enough operations in the current interval, sleep for the rest of the interval. Then
     * add more operations to the queue.
     */
    usecs_delta = WT_TIMEDIFF_US(now, throttle_cfg->last_increment);
    if (usecs_delta < throttle_cfg->usecs_increment) {
        (void)usleep((useconds_t)(throttle_cfg->usecs_increment - usecs_delta));
        throttle_cfg->ops_count = throttle_cfg->ops_per_increment;
        /*
         * After sleeping, set the interval to the current time.
         */
        __wt_epoch(NULL, &throttle_cfg->last_increment);
    } else {
        throttle_cfg->ops_count =
          (usecs_delta * throttle_cfg->ops_per_increment) / throttle_cfg->usecs_increment;
        throttle_cfg->last_increment = now;
    }

    /*
     * Take the minimum so we don't overfill the queue.
     */
    throttle_cfg->ops_count = WT_MIN(throttle_cfg->ops_count, thread->workload->throttle);
}
