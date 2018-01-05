/*-
 * Public Domain 2014-2018 MongoDB, Inc.
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

extern char *__wt_optarg;		/* argument associated with option */

/*
 * testutil_parse_opts --
 *    Parse command line options for a test case.
 */
int
testutil_parse_opts(int argc, char * const *argv, TEST_OPTS *opts)
{
	size_t len;
	int ch;

	opts->preserve = false;
	opts->running = true;
	opts->verbose = false;

	opts->progname = testutil_set_progname(argv);

	while ((ch = __wt_getopt(opts->progname,
		argc, argv, "A:h:n:o:pR:T:t:vW:")) != EOF)
		switch (ch) {
		case 'A': /* Number of append threads */
			opts->n_append_threads = (uint64_t)atoll(__wt_optarg);
			break;
		case 'h': /* Home directory */
			opts->home = dstrdup(__wt_optarg);
			break;
		case 'n': /* Number of records */
			opts->nrecords = (uint64_t)atoll(__wt_optarg);
			break;
		case 'o': /* Number of operations */
			opts->nops = (uint64_t)atoll(__wt_optarg);
			break;
		case 'p': /* Preserve directory contents */
			opts->preserve = true;
			break;
		case 'R': /* Number of reader threads */
			opts->n_read_threads = (uint64_t)atoll(__wt_optarg);
			break;
		case 'T': /* Number of threads */
			opts->nthreads = (uint64_t)atoll(__wt_optarg);
			break;
		case 't': /* Table type */
			switch (__wt_optarg[0]) {
			case 'C':
			case 'c':
				opts->table_type = TABLE_COL;
				break;
			case 'F':
			case 'f':
				opts->table_type = TABLE_FIX;
				break;
			case 'R':
			case 'r':
				opts->table_type = TABLE_ROW;
				break;
			}
			break;
		case 'v':
			opts->verbose = true;
			break;
		case 'W': /* Number of writer threads */
			opts->n_write_threads = (uint64_t)atoll(__wt_optarg);
			break;
		case '?':
		default:
			(void)fprintf(stderr, "usage: %s "
			    "[-A append thread count] "
			    "[-h home] "
			    "[-n record count] "
			    "[-o op count] "
			    "[-p] "
			    "[-R read thread count] "
			    "[-T thread count] "
			    "[-t c|f|r table type] "
			    "[-v] "
			    "[-W write thread count] ",
			    opts->progname);
			return (1);
		}

	/*
	 * Setup the home directory if not explicitly specified. It needs to be
	 * unique for every test or the auto make parallel tester gets upset.
	 */
	if (opts->home == NULL) {
		len = strlen("WT_TEST.")  + strlen(opts->progname) + 10;
		opts->home = dmalloc(len);
		testutil_check(__wt_snprintf(
		    opts->home, len, "WT_TEST.%s", opts->progname));
	}

	/*
	 * Setup the progress file name.
	 */
	len = strlen(opts->home) + 20;
	opts->progress_file_name = dmalloc(len);
	testutil_check(__wt_snprintf(opts->progress_file_name, len,
	    "%s/progress.txt", opts->home));

	/* Setup the default URI string */
	len = strlen("table:") + strlen(opts->progname) + 10;
	opts->uri = dmalloc(len);
	testutil_check(__wt_snprintf(
	    opts->uri, len, "table:%s", opts->progname));

	return (0);
}
