/*-
 * Public Domain 2014-2017 MongoDB, Inc.
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

#include "wt_internal.h"

/*
 * __time_check_monotonic --
 *	Check and prevent time running backward.  If we detect that it has, we
 *	set the time structure to the previous values, making time stand still
 *	until we see a time in the future of the highest value seen so far.
 */
static void
__time_check_monotonic(WT_SESSION_IMPL *session, struct timespec *tsp)
{
	/*
	 * Detect time going backward.  If so, use the last
	 * saved timestamp.
	 */
	if (session == NULL)
		return;

	if (tsp->tv_sec < session->last_epoch.tv_sec ||
	     (tsp->tv_sec == session->last_epoch.tv_sec &&
	     tsp->tv_nsec < session->last_epoch.tv_nsec)) {
		WT_STAT_CONN_INCR(session, time_travel);
		*tsp = session->last_epoch;
	} else
		session->last_epoch = *tsp;
}

/*
 * __wt_epoch --
 *	Return the time since the Epoch, adjusted so it never appears to go
 *	backwards.
 */
void
__wt_epoch(WT_SESSION_IMPL *session, struct timespec *tsp)
    WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
	struct timespec tmp;

	/*
	 * Read into a local variable so that we're comparing the correct
	 * value when we check for monotonic increasing time.  There are
	 * many places we read into an unlocked global variable.
	 */
	__wt_epoch_raw(session, &tmp);
	__time_check_monotonic(session, &tmp);
	*tsp = tmp;
}

/*
 * __wt_seconds --
 *	Return the seconds since the Epoch.
 */
void
__wt_seconds(WT_SESSION_IMPL *session, time_t *timep)
{
	struct timespec t;

	__wt_epoch(session, &t);

	*timep = t.tv_sec;
}
