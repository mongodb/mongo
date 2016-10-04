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

#include "test_util.h"

void (*custom_die)(void) = NULL;

void test_value(int64_t);
void test_spread(int64_t, int64_t, int64_t);

void
test_value(int64_t val)
{
	const uint8_t *cp;
	uint8_t buf[10], *p;
	int64_t sinput, soutput;
	uint64_t uinput, uoutput;
	size_t used_len;

	soutput = 0;	/* -Werror=maybe-uninitialized */
	sinput = val;
	soutput = 0;	/* Make GCC happy. */
	p = buf;
	testutil_check(__wt_vpack_int(&p, sizeof(buf), sinput));
	used_len = (size_t)(p - buf);
	cp = buf;
	testutil_check(__wt_vunpack_int(&cp, used_len, &soutput));
	/* Ensure we got the correct value back */
	if (sinput != soutput) {
		fprintf(stderr, "mismatch %" PRIu64 ", %" PRIu64 "\n",
		    sinput, soutput);
		abort();
	}
	/* Ensure that decoding used the correct amount of buffer */
	if (cp != p) {
		fprintf(stderr,
		    "Unpack consumed wrong size for %" PRId64
		    ", expected %" WT_SIZET_FMT ", got %" WT_SIZET_FMT "\n",
		    sinput, used_len, cp > p ?
		    used_len + (size_t)(cp - p) : /* More than buf used */
		    used_len - (size_t)(p - cp)); /* Less than buf used */
		abort();
	}

	/* Test unsigned, convert negative into bigger positive values */
	uinput = (uint64_t)val;

	p = buf;
	testutil_check(__wt_vpack_uint(&p, sizeof(buf), uinput));
	cp = buf;
	testutil_check(__wt_vunpack_uint(&cp, sizeof(buf), &uoutput));
	/* Ensure we got the correct value back */
	if (sinput != soutput) {
		fprintf(stderr, "mismatch %" PRIu64 ", %" PRIu64 "\n",
		    sinput, soutput);
		abort();
	}
	/* Ensure that decoding used the correct amount of buffer */
	if (cp != p) {
		fprintf(stderr,
		    "Unpack consumed wrong size for %" PRId64
		    ", expected %" WT_SIZET_FMT ", got %" WT_SIZET_FMT "\n",
		    sinput, used_len, cp > p ?
		    used_len + (size_t)(cp - p) :
		    used_len - (size_t)(p - cp));
		abort();
	}
}

void
test_spread(int64_t start, int64_t before, int64_t after)
{
	int64_t i;

	printf(
	    "Testing range: %" PRId64 " to %" PRId64 ". Spread: % " PRId64 "\n",
	    start - before, start + after, before + after);
	for (i = start - before; i < start + after; i++)
		test_value(i);
}

int
main(void)
{
	int64_t i;

	/*
	 * Test all values in a range, to ensure pack/unpack of small numbers
	 * (which most actively use different numbers of bits) works.
	 */
	test_spread(0, 100000, 100000);
	test_spread(INT16_MAX, 1025, 1025);
	test_spread(INT32_MAX, 1025, 1025);
	test_spread(INT64_MAX, 1025, 1025);
	/* Test bigger numbers */
	for (i = INT64_MAX; i > 0; i = i / 2)
		test_spread(i, 1025, 1025);
	printf("\n");

	return (0);
}
