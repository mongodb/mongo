/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <getopt.h>
#include "igzip_lib.h"
#include "test.h"

#define DICT_LEN 32*1024

extern void isal_deflate_hash(struct isal_zstream *stream, uint8_t * dict, int dict_len);

void create_rand_data(uint8_t * data, uint32_t size)
{
	int i;
	for (i = 0; i < size; i++) {
		data[i] = rand() % 256;
	}
}

int main(int argc, char *argv[])
{
	int time = BENCHMARK_TIME;
	struct isal_zstream stream;
	uint8_t dict[DICT_LEN];
	uint32_t dict_len = DICT_LEN;

	stream.level = 0;
	create_rand_data(dict, dict_len);

	struct perf start;
	BENCHMARK(&start, time, isal_deflate_hash(&stream, dict, dict_len));

	printf("igzip_build_hash_table_perf: in_size=%u ", dict_len);
	perf_print(start, (long long)dict_len);

	return 0;
}
