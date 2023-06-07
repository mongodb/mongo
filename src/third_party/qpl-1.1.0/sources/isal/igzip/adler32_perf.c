/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "igzip_lib.h"
#include "test.h"

//#define CACHED_TEST
#ifdef CACHED_TEST
// Cached test, loop many times over small dataset
#define TEST_LEN     8*1024
#define TEST_TYPE_STR "_warm"
#else
// Uncached test.  Pull from large mem base.
#define GT_L3_CACHE  32*1024*1024	/* some number > last level cache */
#define TEST_LEN     (2 * GT_L3_CACHE)
#define TEST_TYPE_STR "_cold"
#endif

#ifndef TEST_SEED
#define TEST_SEED 0x1234
#endif

int main(int argc, char *argv[])
{
	void *buf;
	uint32_t checksum = 0;
	struct perf start;

	printf("adler32_perf:\n");

	if (posix_memalign(&buf, 1024, TEST_LEN)) {
		printf("alloc error: Fail");
		return -1;
	}
	memset(buf, 0, TEST_LEN);

	BENCHMARK(&start, BENCHMARK_TIME, checksum |= isal_adler32(TEST_SEED, buf, TEST_LEN));
	printf("adler32" TEST_TYPE_STR ": ");
	perf_print(start, (long long)TEST_LEN);

	return 0;
}
