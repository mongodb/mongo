/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_nlpo2 --
 *	Return the next-largest power-of-two for a 32-bit unsigned value.
 *
 * In 12 operations, this code computes the next highest power of 2 for a 32-bit
 * integer. The result may be expressed by the formula 1U << (lg(v - 1) + 1).
 * Note that in the edge case where v is 0, it returns 0, which isn't a power of
 * 2; you might append the expression v += (v == 0) to remedy this if it
 * matters.  It would be faster by 2 operations to use the formula and the
 * log base 2 methed that uses a lookup table, but in some situations, lookup
 * tables are not suitable, so the above code may be best. (On a Athlon XP 2100+
 * I've found the above shift-left and then OR code is as fast as using a single
 * BSR assembly language instruction, which scans in reverse to find the highest
 * set bit.) It works by copying the highest set bit to all of the lower bits,
 * and then adding one, which results in carries that set all of the lower bits
 * to 0 and one bit beyond the highest set bit to 1. If the original number was
 * a power of 2, then the decrement will reduce it to one less, so that we round
 * up to the same original value.  Devised by Sean Anderson, Sepember 14, 2001.
 * Pete Hart pointed me to a couple newsgroup posts by him and William Lewis in
 * February of 1997, where they arrive at the same algorithm.
 *	http://graphics.stanford.edu/~seander/bithacks.html
 *	Sean Eron Anderson, seander@cs.stanford.edu
 */
u_int32_t
__wt_nlpo2(u_int32_t v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return (v);
}
