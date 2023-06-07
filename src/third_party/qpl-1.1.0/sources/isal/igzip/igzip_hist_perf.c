/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "igzip_lib.h"
#include "test.h"

#define BUF_SIZE 1024
#define MIN_TEST_LOOPS   8
#ifndef RUN_MEM_SIZE
# define RUN_MEM_SIZE 2000000000
#endif

void print_histogram(struct isal_huff_histogram *histogram)
{
	int i;
	printf("Lit Len histogram");
	for (i = 0; i < ISAL_DEF_LIT_LEN_SYMBOLS; i++) {
		if (i % 16 == 0)
			printf("\n");
		else
			printf(", ");
		printf("%4lu", histogram->lit_len_histogram[i]);
	}
	printf("\n");

	printf("Dist histogram");
	for (i = 0; i < ISAL_DEF_DIST_SYMBOLS; i++) {
		if (i % 16 == 0)
			printf("\n");
		else
			printf(", ");
		printf("%4lu", histogram->dist_histogram[i]);
	}
	printf("\n");
}

int main(int argc, char *argv[])
{
	FILE *in;
	unsigned char *inbuf;
	int iterations, avail_in;
	uint64_t infile_size;
	struct isal_huff_histogram histogram1, histogram2;

	memset(&histogram1, 0, sizeof(histogram1));
	memset(&histogram2, 0, sizeof(histogram2));

	if (argc > 3 || argc < 2) {
		fprintf(stderr, "Usage: igzip_file_perf  infile [outfile]\n"
			"\t - Runs multiple iterations of igzip on a file to "
			"get more accurate time results.\n");
		exit(0);
	}
	in = fopen(argv[1], "rb");
	if (!in) {
		fprintf(stderr, "Can't open %s for reading\n", argv[1]);
		exit(0);
	}

	/* Allocate space for entire input file and output
	 * (assuming some possible expansion on output size)
	 */
	infile_size = get_filesize(in);

	if (infile_size != 0)
		iterations = RUN_MEM_SIZE / infile_size;
	else
		iterations = MIN_TEST_LOOPS;

	if (iterations < MIN_TEST_LOOPS)
		iterations = MIN_TEST_LOOPS;

	inbuf = malloc(infile_size);
	if (inbuf == NULL) {
		fprintf(stderr, "Can't allocate input buffer memory\n");
		exit(0);
	}

	avail_in = fread(inbuf, 1, infile_size, in);
	if (avail_in != infile_size) {
		free(inbuf);
		fprintf(stderr, "Couldn't fit all of input file into buffer\n");
		exit(0);
	}

	struct perf start;
	BENCHMARK(&start, BENCHMARK_TIME,
		  isal_update_histogram(inbuf, infile_size, &histogram1));
	printf("  file %s - in_size=%lu\n", argv[1], infile_size);
	printf("igzip_hist_file: ");
	perf_print(start, (long long)infile_size);

	fclose(in);
	fflush(0);
	free(inbuf);

	return 0;
}
