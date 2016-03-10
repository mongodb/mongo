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

#include "wt_internal.h"

/*
 * This is an implementation of condition variables that automatically adjust
 * the wait time depending on whether the wake is resulting in useful work.
 */

/*
 * __wt_cond_auto_alloc --
 *	Allocate and initialize an automatically adjusting condition variable.
 */
int
__wt_cond_auto_alloc(
    WT_SESSION_IMPL *session, const char *name,
    bool is_signalled, uint64_t min, uint64_t max, WT_CONDVAR **condp)
{
	WT_CONDVAR *cond;

	WT_RET(__wt_cond_alloc(session, name, is_signalled, condp));
	cond = *condp;

	cond->min_wait = min;
	cond->max_wait = max;
	cond->prev_wait = min;

	return (0);
}

/*
 * __wt_cond_auto_signal --
 *	Signal a condition variable.
 */
int
__wt_cond_auto_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond)
{

	WT_ASSERT(session, cond->min_wait != 0);
	return (__wt_cond_signal(session, cond));
}

/*
 * __wt_cond_auto_wait_signal --
 *	Wait on a mutex, optionally timing out.  If we get it before the time
 *	out period expires, let the caller know.
 *	TODO: Can this version of the API be removed, now that we have the
 *	auto adjusting condition variables?
 */
int
__wt_cond_auto_wait_signal(
    WT_SESSION_IMPL *session, WT_CONDVAR *cond, bool progress, bool *signalled)
{
	uint64_t delta;

	/*
	 * Catch cases where this function is called with a condition variable
	 * that was initialized non-auto.
	 */
	WT_ASSERT(session, cond->min_wait != 0);

	WT_STAT_FAST_CONN_INCR(session, cond_auto_wait);
	if (progress)
		cond->prev_wait = cond->min_wait;
	else {
		delta = WT_MAX(1, (cond->max_wait - cond->min_wait) / 10);
		cond->prev_wait = WT_MIN(
		    cond->max_wait, cond->prev_wait + delta);
	}

	WT_RET(__wt_cond_wait_signal(
	    session, cond, cond->prev_wait, signalled));

	if (progress || *signalled)
		WT_STAT_FAST_CONN_INCR(session, cond_auto_wait_reset);
	if (*signalled)
		cond->prev_wait = cond->min_wait;

	return (0);
}

/*
 * __wt_cond_auto_wait --
 *	Wait on a mutex, optionally timing out.  If we get it before the time
 *	out period expires, let the caller know.
 */
int
__wt_cond_auto_wait(
    WT_SESSION_IMPL *session, WT_CONDVAR *cond, bool progress)
{
	bool signalled;

	/*
	 * Call the signal version so the wait period is reset if the
	 * condition is woken explicitly.
	 */
	WT_RET(__wt_cond_auto_wait_signal(session, cond, progress, &signalled));

	return (0);
}

/*
 * __wt_cond_auto_destroy --
 *	Destroy a condition variable.
 */
int
__wt_cond_auto_destroy(WT_SESSION_IMPL *session, WT_CONDVAR **condp)
{
	return (__wt_cond_destroy(session, condp));
}
