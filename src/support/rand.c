/*-
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

#undef	M_W
#define	M_W	(rnd)[0]
#undef	M_Z
#define	M_Z	(rnd)[1]

/*
 * __wt_random_init --
 *	Initialize return of a 32-bit pseudo-random number.
 */
void
__wt_random_init(uint32_t *rnd)
{
	M_W = 521288629;
	M_Z = 362436069;
}

/*
 * __wt_random --
 *	Return a 32-bit pseudo-random number.
 *
 * This is an implementation of George Marsaglia's multiply-with-carry pseudo-
 * random number generator.  Computationally fast, with reasonable randomness
 * properties.
 *
 * We have to be very careful about races here.  Multiple threads can call
 * __wt_random concurrently, and it is okay if those concurrent calls get the
 * same return value.  What is *not* okay is if reading the shared state races
 * with an update and uses two different values for m_w or m_z.  That could
 * result in a value of zero, in which case they would be stuck on zero
 * forever.  Take local copies of the shared values to avoid this.
 */
uint32_t
__wt_random(uint32_t *rnd)
{
	uint32_t w = M_W, z = M_Z;

	M_Z = z = 36969 * (z & 65535) + (z >> 16);
	M_W = w = 18000 * (w & 65535) + (w >> 16);
	return (z << 16) + (w & 65535);
}
