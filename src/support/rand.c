/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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
 * __wt_random --
 *	Return a 32-bit pseudo-random number.
 *
 * This is an implementation of George Marsaglia's multiply-with-carry pseudo-
 * random number generator.  Computationally fast, with reasonable randomness
 * properties.
 *
 * We have to be very careful about races here.  Multiple threads can call
 * __wt_random concurrently, and it is okay if those concurrent calls get the
 * same return value.  What is *not* okay is if the reading the shared
 * variables races with an update and uses two different values for m_w or m_z.
 * In that case, the result could be zero, in which case they would be stuck on
 * zero forever.  Take local copies of the shared values to avoid this.
 */
uint32_t
__wt_random(void)
{
	static uint32_t m_w = 521288629;
	static uint32_t m_z = 362436069;
	uint32_t w = m_w, z = m_z;

	m_z = z = 36969 * (z & 65535) + (z >> 16);
	m_w = w = 18000 * (w & 65535) + (w >> 16);
	return (z << 16) + (w & 65535);
}
