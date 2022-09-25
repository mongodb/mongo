/*-
 * Public Domain 2014-present MongoDB, Inc.
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
#define DIR_STORE "dir_store"

extern char *__wt_optarg; /* argument associated with option */

/*
 * parse_tiered_opts --
 *     Parse command line options for the tiered storage configurations.
 */
static inline void
parse_tiered_opts(int argc, char *const *argv, TEST_OPTS *opts)
{
    int index, i;
    int number_of_tiered_options;

    number_of_tiered_options = 0;
    index = 0;
    opts->tiered_storage = false;
    opts->tiered_storage_source = NULL;

    for (i = 1; i < argc; i++) {
        /* Tiered storage command line options starts with -P. */
        if (strstr(argv[i], "-P")) {
            /* Many more options to come here. */
            if (argv[i][2] == 'o') {
                if (argv[i + 1] == NULL)
                    testutil_die(
                      EINVAL, "%s option requires an argument %s", opts->progname, argv[i]);
                number_of_tiered_options += 2;
            } else if (argv[i][2] == 'T') {
                /* This parsing is different because -PT does not accept any arguments. */
                ++number_of_tiered_options;
                opts->tiered_storage = true;
            }
        }
    }

    /* Return from here if tiered arguments are not passed. */
    if (!opts->tiered_storage) {
        if (number_of_tiered_options == 0)
            return;
        else
            testutil_die(
              EINVAL, "Error - Tiered storage command line arguments are passed without -PT.");
    }

    opts->nargc = argc - number_of_tiered_options;

    /* Allocate the memory for the new argv without tiered options. */
    opts->nargv = dmalloc(((size_t)opts->nargc + 1) * sizeof(char *));

    /* Copy other command line arguments except tiered. */
    for (i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-Po") == 0) {
            opts->tiered_storage_source = dstrdup(argv[i + 1]);
            /* Move the index because this option has an argument. */
            i++;
        } else if (strcmp(argv[i], "-PT") == 0)
            continue;
        else
            opts->nargv[index++] = dstrdup(argv[i]);
    }

    testutil_assert(index == opts->nargc);
    /*
     * Allocating an extra empty space at the end of the new argv just to replicate the system argv
     * implementation.
     */
    opts->nargv[index++] = dstrdup("");

    if (opts->tiered_storage_source == NULL)
        opts->tiered_storage_source = dstrdup(DIR_STORE);
}

/*
 * testutil_parse_opts --
 *     Parse command line options for a test case.
 */
int
testutil_parse_opts(int argc, char *const *argv, TEST_OPTS *opts)
{
    size_t len;
    int ch;

    opts->nargc = 0;
    opts->nargv = NULL;
    opts->do_data_ops = false;
    opts->preserve = false;
    opts->running = true;
    opts->verbose = false;

    opts->argv0 = argv[0];
    opts->progname = testutil_set_progname(argv);

    testutil_print_command_line(argc, argv);

    parse_tiered_opts(argc, argv, opts);

    if (!opts->tiered_storage) {
        opts->nargv = (char **)argv;
        opts->nargc = argc;
    }

    while (
      (ch = __wt_getopt(opts->progname, opts->nargc, opts->nargv, "A:b:dh:n:o:pR:T:t:vW:")) != EOF)
        switch (ch) {
        case 'A': /* Number of append threads */
            opts->n_append_threads = (uint64_t)atoll(__wt_optarg);
            break;
        case 'b': /* Build directory */
            opts->build_dir = dstrdup(__wt_optarg);
            break;
        case 'd': /* Use data in multi-threaded test programs */
            opts->do_data_ops = true;
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
            (void)fprintf(stderr,
              "usage: %s "
              "[-A append thread count] "
              "[-b build directory] "
              "[-d add data] "
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
     * Setup the home directory if not explicitly specified. It needs to be unique for every test or
     * the auto make parallel tester gets upset.
     */
    if (opts->home == NULL) {
        len = strlen("WT_TEST.") + strlen(opts->progname) + 10;
        opts->home = dmalloc(len);
        testutil_check(__wt_snprintf(opts->home, len, "WT_TEST.%s", opts->progname));
    }

    /*
     * Setup the progress file name.
     */
    len = strlen(opts->home) + 20;
    opts->progress_file_name = dmalloc(len);
    testutil_check(__wt_snprintf(opts->progress_file_name, len, "%s/progress.txt", opts->home));

    /* Setup the default URI string */
    len = strlen("table:") + strlen(opts->progname) + 10;
    opts->uri = dmalloc(len);
    testutil_check(__wt_snprintf(opts->uri, len, "table:%s", opts->progname));

    return (0);
}
