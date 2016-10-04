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

#ifdef __WIREDTIGER_UNUSED__

/*
 * __wt_nlpo2_round --
 *	Round up to the next-largest power-of-two for a 32-bit unsigned value.
 *
 * In 12 operations, this code computes the next highest power of 2 for a 32-bit
 * integer. The result may be expressed by the formula 1U << (lg(v - 1) + 1).
 * Note that in the edge case where v is 0, it returns 0, which isn't a power of
 * 2; you might append the expression v += (v == 0) to remedy this if it
 * matters.  It would be faster by 2 operations to use the formula and the
 * log base 2 method that uses a lookup table, but in some situations, lookup
 * tables are not suitable, so the above code may be best. (On a Athlon XP 2100+
 * I've found the above shift-left and then OR code is as fast as using a single
 * BSR assembly language instruction, which scans in reverse to find the highest
 * set bit.) It works by copying the highest set bit to all of the lower bits,
 * and then adding one, which results in carries that set all of the lower bits
 * to 0 and one bit beyond the highest set bit to 1. If the original number was
 * a power of 2, then the decrement will reduce it to one less, so that we round
 * up to the same original value.  Devised by Sean Anderson, September 14, 2001.
 * Pete Hart pointed me to a couple newsgroup posts by him and William Lewis in
 * February of 1997, where they arrive at the same algorithm.
 *	http://graphics.stanford.edu/~seander/bithacks.html
 *	Sean Eron Anderson, seander@cs.stanford.edu
 */
uint32_t
__wt_nlpo2_round(uint32_t v)
{
	v--;				/* If v is a power-of-two, return it. */
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return (v + 1);
}

/*
 * __wt_nlpo2 --
 *	Return the next largest power-of-two.
 */
uint32_t
__wt_nlpo2(uint32_t v)
{
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return (v + 1);
}
#endif /* __WIREDTIGER_UNUSED__ */

/*
 * __wt_log2_int --
 *	Find the log base 2 of an integer in O(N) operations;
 *	http://graphics.stanford.edu/~seander/bithacks.html
 */
uint32_t
__wt_log2_int(uint32_t n)
{
	uint32_t l = 0;

	while (n >>= 1)
		l++;
	return (l);
}

/*
 * __wt_ispo2 --
 *	Return if a number is a power-of-two.
 */
bool
__wt_ispo2(uint32_t v)
{
	/*
	 * Only numbers that are powers of two will satisfy the relationship
	 * (v & (v - 1) == 0).
	 *
	 * However n must be positive, this returns 0 as a power of 2; to fix
	 * that, use: (! (v & (v - 1)) && v)
	 */
	return ((v & (v - 1)) == 0);
}

/*
 * __wt_rduppo2 --
 *	Round the given int up to the next multiple of N, where N is power of 2.
 */
uint32_t
__wt_rduppo2(uint32_t n, uint32_t po2)
{
	uint32_t bits, res;

	if (__wt_ispo2(po2)) {
		bits = __wt_log2_int(po2);
		res = (((n - 1) >> bits) + 1) << bits;
	} else
		res = 0;
	return (res);
}
