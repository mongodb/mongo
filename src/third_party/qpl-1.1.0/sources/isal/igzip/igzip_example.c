/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "igzip_lib.h"

#define BUF_SIZE 8192
#ifndef LEVEL
# define LEVEL 0
#else
# define LEVEL 1
#endif

struct isal_zstream stream;

int main(int argc, char *argv[])
{
	uint8_t inbuf[BUF_SIZE], outbuf[BUF_SIZE];
	FILE *in, *out;

	if (argc != 3) {
		fprintf(stderr, "Usage: igzip_example infile outfile\n");
		exit(0);
	}
	in = fopen(argv[1], "rb");
	if (!in) {
		fprintf(stderr, "Can't open %s for reading\n", argv[1]);
		exit(0);
	}
	out = fopen(argv[2], "wb");
	if (!out) {
		fprintf(stderr, "Can't open %s for writing\n", argv[2]);
		exit(0);
	}

	printf("igzip_example\nWindow Size: %d K\n", IGZIP_HIST_SIZE / 1024);
	fflush(0);

	isal_deflate_init(&stream);
	stream.end_of_stream = 0;
	stream.flush = NO_FLUSH;

	if (LEVEL == 1) {
		stream.level = 1;
		stream.level_buf = malloc(ISAL_DEF_LVL1_DEFAULT);
		stream.level_buf_size = ISAL_DEF_LVL1_DEFAULT;
		if (stream.level_buf == 0) {
			printf("Failed to allocate level compression buffer\n");
			exit(0);
		}
	}

	do {
		stream.avail_in = (uint32_t) fread(inbuf, 1, BUF_SIZE, in);
		stream.end_of_stream = feof(in) ? 1 : 0;
		stream.next_in = inbuf;
		do {
			stream.avail_out = BUF_SIZE;
			stream.next_out = outbuf;

			isal_deflate(&stream);

			fwrite(outbuf, 1, BUF_SIZE - stream.avail_out, out);
		} while (stream.avail_out == 0);

		assert(stream.avail_in == 0);
	} while (stream.internal_state.state != ZSTATE_END);

	fclose(out);
	fclose(in);

	printf("End of igzip_example\n\n");
	return 0;
}
