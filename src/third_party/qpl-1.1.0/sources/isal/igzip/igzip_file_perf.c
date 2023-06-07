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
#include <getopt.h>
#include "igzip_lib.h"
#include "test.h"

#define BUF_SIZE 1024

int level_size_buf[10] = {
#ifdef ISAL_DEF_LVL0_DEFAULT
	ISAL_DEF_LVL0_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL1_DEFAULT
	ISAL_DEF_LVL1_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL2_DEFAULT
	ISAL_DEF_LVL2_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL3_DEFAULT
	ISAL_DEF_LVL3_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL4_DEFAULT
	ISAL_DEF_LVL4_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL5_DEFAULT
	ISAL_DEF_LVL5_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL6_DEFAULT
	ISAL_DEF_LVL6_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL7_DEFAULT
	ISAL_DEF_LVL7_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL8_DEFAULT
	ISAL_DEF_LVL8_DEFAULT,
#else
	0,
#endif
#ifdef ISAL_DEF_LVL9_DEFAULT
	ISAL_DEF_LVL9_DEFAULT,
#else
	0,
#endif
};

int usage(void)
{
	fprintf(stderr,
		"Usage: igzip_file_perf [options] <infile>\n"
		"  -h        help\n"
		"  -X        use compression level X with 0 <= X <= 1\n"
		"  -b <size> input buffer size, 0 buffers all the input\n"
		"  -i <time> time in seconds to benchmark (at least 0)\n"
		"  -o <file> output file for compresed data\n"
		"  -d <file> dictionary file used by compression\n"
		"  -w <size> log base 2 size of history window, between 8 and 15\n");

	exit(0);
}

void deflate_perf(struct isal_zstream *stream, uint8_t * inbuf, size_t infile_size,
		  size_t inbuf_size, uint8_t * outbuf, size_t outbuf_size, int level,
		  uint8_t * level_buf, int level_size, uint32_t hist_bits, uint8_t * dictbuf,
		  size_t dictfile_size, struct isal_dict *dict_str,
		  struct isal_hufftables *hufftables_custom)
{
	int avail_in;
	isal_deflate_init(stream);
	stream->level = level;
	stream->level_buf = level_buf;
	stream->level_buf_size = level_size;

	if (COMP_OK != isal_deflate_reset_dict(stream, dict_str))
		if (dictbuf != NULL)
			isal_deflate_set_dict(stream, dictbuf, dictfile_size);

	stream->end_of_stream = 0;
	stream->flush = NO_FLUSH;
	stream->next_out = outbuf;
	stream->avail_out = outbuf_size;
	stream->next_in = inbuf;
	if (hufftables_custom != NULL)
		stream->hufftables = hufftables_custom;
	stream->hist_bits = hist_bits;
	avail_in = infile_size;

	while (avail_in > 0) {
		stream->avail_in = avail_in >= inbuf_size ? inbuf_size : avail_in;
		avail_in -= inbuf_size;

		if (avail_in <= 0)
			stream->end_of_stream = 1;

		isal_deflate(stream);

		if (stream->avail_in != 0)
			break;
	}
}

int main(int argc, char *argv[])
{
	FILE *in = NULL, *out = NULL, *dict = NULL;
	unsigned char *inbuf, *outbuf, *level_buf = NULL, *dictbuf = NULL;
	int c, time = BENCHMARK_TIME, inbuf_size = 0;
	size_t infile_size, outbuf_size, dictfile_size;
	struct isal_huff_histogram histogram;
	struct isal_hufftables hufftables_custom;
	int level = 0, level_size = 0;
	char *in_file_name = NULL, *out_file_name = NULL, *dict_file_name = NULL;
	uint32_t hist_bits = 0;
	struct isal_zstream stream;

	while ((c = getopt(argc, argv, "h0123456789i:b:o:d:w:")) != -1) {
		if (c >= '0' && c <= '9') {
			if (c > '0' + ISAL_DEF_MAX_LEVEL)
				usage();
			else {
				level = c - '0';
				level_size = level_size_buf[level];
			}
			continue;
		}

		switch (c) {
		case 'o':
			out_file_name = optarg;
			break;
		case 'd':
			dict_file_name = optarg;
			break;
		case 'i':
			time = atoi(optarg);
			if (time < 0)
				usage();
			break;
		case 'b':
			inbuf_size = atoi(optarg);
			break;
		case 'w':
			hist_bits = atoi(optarg);
			if (hist_bits > 15 || hist_bits < 8)
				usage();
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	if (optind < argc) {
		in_file_name = argv[optind];
		in = fopen(in_file_name, "rb");
	} else
		usage();

	if (!in) {
		fprintf(stderr, "Can't open %s for reading\n", in_file_name);
		exit(0);
	}
	if (out_file_name != NULL) {
		out = fopen(out_file_name, "wb");
		if (!out) {
			fprintf(stderr, "Can't open %s for writing\n", out_file_name);
			exit(0);
		}
		printf("outfile=%s\n", out_file_name);
	}

	if (dict_file_name != NULL) {
		dict = fopen(dict_file_name, "rb");
		if (!dict) {
			fprintf(stderr, "Can't open %s for reading\n", dict_file_name);
			exit(0);
		}
		printf("outfile=%s\n", dict_file_name);
	}

	if (hist_bits == 0)
		printf("Window Size: %d K\n", IGZIP_HIST_SIZE / 1024);

	else if (hist_bits < 10)
		printf("Window Size: %.2f K\n", 1.0 * (1 << hist_bits) / 1024);
	else
		printf("Window Size: %d K\n", (1 << hist_bits) / 1024);

	printf("igzip_file_perf: \n");
	fflush(0);

	/* Allocate space for entire input file and output
	 * (assuming some possible expansion on output size)
	 */
	infile_size = get_filesize(in);

	outbuf_size = 2 * infile_size + BUF_SIZE;

	dictfile_size = (dict_file_name != NULL) ? get_filesize(dict) : 0;

	inbuf = malloc(infile_size);
	if (inbuf == NULL) {
		fprintf(stderr, "Can't allocate input buffer memory\n");
		exit(0);
	}
	outbuf = malloc(outbuf_size);
	if (outbuf == NULL) {
		fprintf(stderr, "Can't allocate output buffer memory\n");
		exit(0);
	}

	if (dictfile_size != 0) {
		dictbuf = malloc(dictfile_size);
		if (dictbuf == NULL) {
			fprintf(stderr, "Can't allocate dictionary buffer memory\n");
			exit(0);
		}
	}

	if (level_size != 0) {
		level_buf = malloc(level_size);
		if (level_buf == NULL) {
			fprintf(stderr, "Can't allocate level buffer memory\n");
			exit(0);
		}
	}

	inbuf_size = inbuf_size ? inbuf_size : infile_size;

	printf("igzip_file_perf: %s\n", in_file_name);

	/* Read complete input file into buffer */
	stream.avail_in = (uint32_t) fread(inbuf, 1, infile_size, in);
	if (stream.avail_in != infile_size) {
		fprintf(stderr, "Couldn't fit all of input file into buffer\n");
		exit(0);
	}

	/* Read complete dictionary into buffer */
	if ((dictfile_size != 0) && (dictfile_size != fread(dictbuf, 1, dictfile_size, dict))) {
		fprintf(stderr, "Couldn't fit all of dictionary file into buffer\n");
		exit(0);
	}

	struct isal_dict dict_str;
	stream.level = level;
	isal_deflate_process_dict(&stream, &dict_str, dictbuf, dictfile_size);

	struct perf start;
	if (time > 0) {
		BENCHMARK(&start, time,
			  deflate_perf(&stream, inbuf, infile_size, inbuf_size, outbuf,
				       outbuf_size, level, level_buf, level_size, hist_bits,
				       dictbuf, dictfile_size, &dict_str, NULL));
	} else {
		deflate_perf(&stream, inbuf, infile_size, inbuf_size, outbuf, outbuf_size,
			     level, level_buf, level_size, hist_bits, dictbuf,
			     dictfile_size, &dict_str, NULL);
	}
	if (stream.avail_in != 0) {
		fprintf(stderr, "Could not compress all of inbuf\n");
		exit(0);
	}

	printf("  file %s - in_size=%lu out_size=%d ratio=%3.1f%%",
	       in_file_name, infile_size, stream.total_out,
	       100.0 * stream.total_out / infile_size);

	if (level == 0) {
		memset(&histogram, 0, sizeof(histogram));

		isal_update_histogram(inbuf, infile_size, &histogram);
		isal_create_hufftables(&hufftables_custom, &histogram);

		deflate_perf(&stream, inbuf, infile_size, inbuf_size, outbuf, outbuf_size,
			     level, level_buf, level_size, hist_bits, dictbuf,
			     dictfile_size, &dict_str, &hufftables_custom);

		printf(" ratio_custom=%3.1f%%", 100.0 * stream.total_out / infile_size);
	}
	printf("\n");

	if (stream.avail_in != 0) {
		fprintf(stderr, "Could not compress all of inbuf\n");
		exit(0);
	}

	printf("igzip_file: ");
	perf_print(start, (long long)infile_size);

	if (argc > 2 && out) {
		printf("writing %s\n", out_file_name);
		fwrite(outbuf, 1, stream.total_out, out);
		fclose(out);
	}

	fclose(in);
	printf("End of igzip_file_perf\n\n");
	fflush(0);
	return 0;
}
