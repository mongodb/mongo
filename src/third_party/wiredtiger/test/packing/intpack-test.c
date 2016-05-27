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

int
main(void)
{
	const uint8_t *cp;
	uint8_t buf[10], *p;
	uint64_t ncalls, r, r2, s;
	int i;

	ncalls = 0;

	for (i = 0; i < 10000000; i++) {
		for (s = 0; s < 50; s += 5) {
			++ncalls;
			r = 1ULL << s;

#if 1
			p = buf;
			testutil_check(__wt_vpack_uint(&p, sizeof(buf), r));
			cp = buf;
			testutil_check(
			    __wt_vunpack_uint(&cp, sizeof(buf), &r2));
#else
			/*
			 * Note: use memmove for comparison because GCC does
			 * aggressive optimization of memcpy and it's difficult
			 * to measure anything.
			 */
			p = buf;
			memmove(p, &r, sizeof(r));
			cp = buf;
			memmove(&r2, cp, sizeof(r2));
#endif
			if (r != r2) {
				fprintf(stderr, "mismatch!\n");
				break;
			}
		}
	}

	printf("Number of calls: %llu\n", (unsigned long long)ncalls);

	return (0);
}
