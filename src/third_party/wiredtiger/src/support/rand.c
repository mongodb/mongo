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
 * This is an implementation of George Marsaglia's multiply-with-carry pseudo-
 * random number generator.  Computationally fast, with reasonable randomness
 * properties, and a claimed period of > 2^60.
 *
 * Be very careful about races here. Multiple threads can call __wt_random
 * concurrently, and it is okay if those concurrent calls get the same return
 * value. What is *not* okay is if reading/writing the shared state races and
 * uses two different values for m_w or m_z. That can result in a stored value
 * of zero, in which case they will be stuck on zero forever. Take a local copy
 * of the values to avoid that, and read/write in atomic, 8B chunks.
 */
#undef	M_W
#define	M_W(r)	r.x.w
#undef	M_Z
#define	M_Z(r)	r.x.z

/*
 * __wt_random_init --
 *	Initialize return of a 32-bit pseudo-random number.
 */
void
__wt_random_init(WT_RAND_STATE volatile * rnd_state)
{
	WT_RAND_STATE rnd;

	M_W(rnd) = 521288629;
	M_Z(rnd) = 362436069;
	*rnd_state = rnd;
}

/*
 * __wt_random_init_seed --
 *	Initialize the state of a 32-bit pseudo-random number.
 * Use this, instead of __wt_random_init if we are running with multiple
 * threads and we want each thread to initialize its own random state based
 * on a different random seed.
 */
int
__wt_random_init_seed(
    WT_SESSION_IMPL *session, WT_RAND_STATE volatile * rnd_state)
{
	struct timespec ts;
	WT_RAND_STATE rnd;

	WT_RET(__wt_epoch(session, &ts));
	M_W(rnd) = (uint32_t)(ts.tv_nsec + 521288629);
	M_Z(rnd) = (uint32_t)(ts.tv_nsec + 362436069);

	*rnd_state = rnd;

	return (0);
}

/*
 * __wt_random --
 *	Return a 32-bit pseudo-random number.
 */
uint32_t
__wt_random(WT_RAND_STATE volatile * rnd_state)
{
	WT_RAND_STATE rnd;
	uint32_t w, z;

	/*
	 * Take a copy of the random state so we can ensure that the
	 * calculation operates on the state consistently regardless of
	 * concurrent calls with the same random state.
	 */
	rnd = *rnd_state;
	w = M_W(rnd);
	z = M_Z(rnd);

	/*
	 * Check if the value goes to 0 (from which we won't recover), and reset
	 * to the initial state. This has additional benefits if a caller fails
	 * to initialize the state, or initializes with a seed that results in a
	 * short period.
	 */
	if (z == 0 || w == 0) {
		__wt_random_init(&rnd);
		w = M_W(rnd);
		z = M_Z(rnd);
	}

	M_Z(rnd) = z = 36969 * (z & 65535) + (z >> 16);
	M_W(rnd) = w = 18000 * (w & 65535) + (w >> 16);
	*rnd_state = rnd;

	return ((z << 16) + (w & 65535));
}
