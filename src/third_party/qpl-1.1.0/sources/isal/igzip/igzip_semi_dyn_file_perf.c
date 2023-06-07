/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "igzip_lib.h"
#include "test.h"

#define MIN_BUF_SIZE    (4 * 1024)
#define MIN_TEST_LOOPS   10
#ifndef RUN_MEM_SIZE
# define RUN_MEM_SIZE 500000000
#endif

#define DEFAULT_SEG_SIZE    (512 * 1024)
#define DEFAULT_SAMPLE_SIZE (32 * 1024)

int usage(void)
{
	fprintf(stderr,
		"Usage: igzip_semi_dynamic [options] <infile>\n"
		"  -h        help\n"
		"  -v        (don't) validate output by inflate and compare\n"
		"  -t <type> 1:stateless 0:(default)stateful\n"
		"  -c <size> chunk size default=%d\n"
		"  -s <size> sample size default=%d\n"
		"  -o <file> output file\n", DEFAULT_SEG_SIZE, DEFAULT_SAMPLE_SIZE);
	exit(0);
}

int str_to_i(char *s)
{
#define ARG_MAX 32

	int i = atoi(s);
	int len = strnlen(s, ARG_MAX);
	if (len < 2 || len == ARG_MAX)
		return i;

	switch (s[len - 1]) {
	case 'k':
		i *= 1024;
		break;
	case 'K':
		i *= 1000;
		break;
	case 'm':
		i *= (1024 * 1024);
		break;
	case 'M':
		i *= (1000 * 1000);
		break;
	case 'g':
		i *= (1024 * 1024 * 1024);
		break;
	case 'G':
		i *= (1000 * 1000 * 1000);
		break;
	}
	return i;
}

void semi_dyn_stateless_perf(struct isal_zstream *stream, uint8_t * inbuf,
			     uint64_t infile_size, uint8_t * outbuf, uint64_t outbuf_size,
			     int segment_size, int hist_size)
{
	struct isal_huff_histogram histogram;
	struct isal_hufftables hufftable;

	isal_deflate_stateless_init(stream);
	stream->end_of_stream = 0;
	stream->flush = FULL_FLUSH;
	stream->next_in = inbuf;
	stream->next_out = outbuf;
	int remaining = infile_size;
	int chunk_size = segment_size;

	while (remaining > 0) {
		// Generate custom hufftables on sample
		memset(&histogram, 0, sizeof(struct isal_huff_histogram));
		if (remaining < segment_size * 2) {
			chunk_size = remaining;
			stream->end_of_stream = 1;
		}
		int hist_rem = (hist_size > chunk_size) ? chunk_size : hist_size;
		isal_update_histogram(stream->next_in, hist_rem, &histogram);

		if (hist_rem == chunk_size)
			isal_create_hufftables_subset(&hufftable, &histogram);
		else
			isal_create_hufftables(&hufftable, &histogram);

		// Compress with custom table
		stream->avail_in = chunk_size;
		stream->avail_out = chunk_size + 8 * (1 + (chunk_size >> 16));
		stream->hufftables = &hufftable;
		remaining -= chunk_size;
		isal_deflate_stateless(stream);
		if (stream->avail_in != 0)
			break;
	}
}

void semi_dyn_stateful_perf(struct isal_zstream *stream, uint8_t * inbuf,
			    uint64_t infile_size, uint8_t * outbuf, uint64_t outbuf_size,
			    int segment_size, int hist_size)
{
	struct isal_huff_histogram histogram;
	struct isal_hufftables hufftable;

	isal_deflate_init(stream);
	stream->end_of_stream = 0;
	stream->flush = SYNC_FLUSH;
	stream->next_in = inbuf;
	stream->next_out = outbuf;
	stream->avail_out = outbuf_size;
	int remaining = infile_size;
	int chunk_size = segment_size;

	while (remaining > 0) {
		// Generate custom hufftables on sample
		memset(&histogram, 0, sizeof(struct isal_huff_histogram));
		if (remaining < segment_size * 2) {
			chunk_size = remaining;
			stream->end_of_stream = 1;
		}
		int hist_rem = (hist_size > chunk_size) ? chunk_size : hist_size;
		isal_update_histogram(stream->next_in, hist_rem, &histogram);

		if (hist_rem == chunk_size)
			isal_create_hufftables_subset(&hufftable, &histogram);
		else
			isal_create_hufftables(&hufftable, &histogram);

		// Compress with custom table
		stream->avail_in = chunk_size;
		stream->hufftables = &hufftable;
		remaining -= chunk_size;
		isal_deflate(stream);
		if (stream->internal_state.state != ZSTATE_NEW_HDR)
			break;
	}
}

int main(int argc, char *argv[])
{
	FILE *in = stdin, *out = NULL;
	unsigned char *inbuf, *outbuf;
	int i = 0, c;
	uint64_t infile_size, outbuf_size;
	int segment_size = DEFAULT_SEG_SIZE;
	int sample_size = DEFAULT_SAMPLE_SIZE;
	int check_output = 1;
	int do_stateless = 0, do_stateful = 1;
	int ret = 0;
	char *out_file_name = NULL;
	struct isal_zstream stream;

	while ((c = getopt(argc, argv, "vht:c:s:o:")) != -1) {
		switch (c) {
		case 'v':
			check_output ^= 1;
			break;
		case 't':
			if (atoi(optarg) == 1) {
				do_stateful = 0;
				do_stateless = 1;
			}
			break;
		case 'c':
			segment_size = str_to_i(optarg);
			break;
		case 's':
			sample_size = str_to_i(optarg);
			break;
		case 'o':
			out_file_name = optarg;
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	// Open input file
	if (optind < argc) {
		if (!(in = fopen(argv[optind], "rb"))) {
			fprintf(stderr, "Can't open %s for reading\n", argv[optind]);
			exit(1);
		}
	} else
		usage();

	// Optionally open output file
	if (out_file_name != NULL) {
		if (!(out = fopen(out_file_name, "wb"))) {
			fprintf(stderr, "Can't open %s for writing\n", out_file_name);
			exit(1);
		}
	}

	printf("Window Size: %d K\n", IGZIP_HIST_SIZE / 1024);

	/*
	 * Allocate space for entire input file and output
	 * (assuming some possible expansion on output size)
	 */
	infile_size = get_filesize(in);
	if (infile_size == 0) {
		printf("Input file has zero length\n");
		usage();
	}

	outbuf_size = infile_size * 1.30 > MIN_BUF_SIZE ? infile_size * 1.30 : MIN_BUF_SIZE;

	if (NULL == (inbuf = malloc(infile_size))) {
		fprintf(stderr, "Can't allocate input buffer memory\n");
		exit(0);
	}
	if (NULL == (outbuf = malloc(outbuf_size))) {
		fprintf(stderr, "Can't allocate output buffer memory\n");
		exit(0);
	}

	int hist_size = sample_size > segment_size ? segment_size : sample_size;

	printf("semi-dynamic sample=%d segment=%d %s\n", hist_size, segment_size,
	       do_stateful ? "stateful" : "stateless");
	printf("igzip_file_perf: %s\n", argv[optind]);

	// Read complete input file into buffer
	stream.avail_in = (uint32_t) fread(inbuf, 1, infile_size, in);
	if (stream.avail_in != infile_size) {
		fprintf(stderr, "Couldn't fit all of input file into buffer\n");
		exit(0);
	}

	struct perf start;

	if (do_stateful) {
		BENCHMARK(&start, BENCHMARK_TIME,
			  semi_dyn_stateful_perf(&stream, inbuf, infile_size, outbuf,
						 outbuf_size, segment_size, hist_size)
		    );
	}

	if (do_stateless) {
		BENCHMARK(&start, BENCHMARK_TIME,
			  semi_dyn_stateless_perf(&stream, inbuf, infile_size, outbuf,
						  outbuf_size, segment_size, hist_size));
	}

	if (stream.avail_in != 0) {
		printf("Could not compress all of inbuf\n");
		ret = 1;
	}

	printf("  file %s - in_size=%lu out_size=%d iter=%d ratio=%3.1f%%\n", argv[optind],
	       infile_size, stream.total_out, i, 100.0 * stream.total_out / infile_size);

	printf("igzip_semi_dyn_file: ");
	perf_print(start, (long long)infile_size);

	if (out != NULL) {
		printf("writing %s\n", out_file_name);
		fwrite(outbuf, 1, stream.total_out, out);
		fclose(out);
	}

	fclose(in);

	if (check_output) {
		unsigned char *inflate_buf;
		struct inflate_state istate;

		if (NULL == (inflate_buf = malloc(infile_size))) {
			fprintf(stderr, "Can't allocate reconstruct buffer memory\n");
			exit(0);
		}
		isal_inflate_init(&istate);
		istate.next_in = outbuf;
		istate.avail_in = stream.total_out;
		istate.next_out = inflate_buf;
		istate.avail_out = infile_size;
		int check = isal_inflate(&istate);

		if (memcmp(inflate_buf, inbuf, infile_size)) {
			printf("inflate check Fail\n");
			printf(" ret %d total_inflate=%d\n", check, istate.total_out);
			for (i = 0; i < infile_size; i++) {
				if (inbuf[i] != inflate_buf[i]) {
					printf("  first diff at offset=%d\n", i);
					break;
				}
			}
			ret = 1;
		} else
			printf("inflate check Pass\n");
		free(inflate_buf);
	}

	printf("End of igzip_semi_dyn_file_perf\n\n");
	return ret;
}
