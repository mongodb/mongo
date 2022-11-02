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
extern int __wt_optind;
extern int __wt_optopt;
extern int __wt_optreset;

/*
 * This is called when parsing a sub-option like the 'o' in -Po, which expects an argument. We need
 * to update the option argument to point to that argument.
 */
#define EXPECT_OPTIONAL_ARG_IN_SUB_PARSE(opts)                            \
    do {                                                                  \
        if (*__wt_optarg == '\0') {                                       \
            /* If we change option indicator, we need to reset getopt. */ \
            __wt_optarg = opts->argv[__wt_optind++];                      \
            __wt_optreset = 1;                                            \
        }                                                                 \
    } while (0)

/*
 * parse_tiered_opt --
 *     Parse a command line option for the tiered storage configurations.
 */
static inline void
parse_tiered_opt(TEST_OPTS *opts)
{
    switch (*__wt_optarg++) {
    case 'o':
        EXPECT_OPTIONAL_ARG_IN_SUB_PARSE(opts);
        if (__wt_optarg == NULL || *__wt_optarg == '\0')
            testutil_die(EINVAL, "-Po option requires an argument");
        opts->tiered_storage_source = dstrdup(__wt_optarg);
        break;
    case 'T':
        opts->tiered_storage = true;
        break;
    }
}

/*
 * testutil_parse_begin_opt --
 *     Start a set of calls to parse single command line options.
 */
void
testutil_parse_begin_opt(int argc, char *const *argv, const char *getopts_string, TEST_OPTS *opts)
{
    opts->argc = 0;
    opts->argv = NULL;
    opts->do_data_ops = false;
    opts->preserve = false;
    opts->running = true;
    opts->verbose = false;

    opts->argv0 = argv[0];
    opts->progname = testutil_set_progname(argv);
    opts->getopts_string = getopts_string;

    testutil_print_command_line(argc, argv);

    opts->argv = (char **)argv;
    opts->argc = argc;

#define USAGE_STR(ch, usage) ((strchr(getopts_string, (ch)) == NULL) ? "" : (usage))

    testutil_check(__wt_snprintf(opts->usage, sizeof(opts->usage), "%s%s%s%s%s%s%s%s%s%s%s%s%s",
      USAGE_STR('A', " [-A append thread count]"), USAGE_STR('b', " [-b build directory]"),
      USAGE_STR('d', " [-d add data]"), USAGE_STR('h', " [-h home]"),
      USAGE_STR('n', " [-n record count]"), USAGE_STR('o', " [-o op count]"),
      USAGE_STR('P', " [-PT] [-Po storage source]"), USAGE_STR('p', " [-p]"),
      USAGE_STR('R', " [-R read thread count]"), USAGE_STR('T', " [-T thread count]"),
      USAGE_STR('t', " [-t c|f|r table type]"), USAGE_STR('v', " [-v]"),
      USAGE_STR('W', " [-W write thread count]")));
}

/*
 * testutil_parse_end_opt --
 *     Finish a set of calls to parse single command line options.
 */
void
testutil_parse_end_opt(TEST_OPTS *opts)
{
    size_t len;

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

    if (opts->tiered_storage && opts->tiered_storage_source == NULL)
        opts->tiered_storage_source = dstrdup(DIR_STORE);
}

/*
 * testutil_parse_single_opt --
 *     Parse a single command line option for a test case.
 */
int
testutil_parse_single_opt(TEST_OPTS *opts, int ch)
{
    if (ch == '?' || strchr(opts->getopts_string, ch) == NULL)
        return (1);

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
    case 'P': /* Tiered storage options follow */
        parse_tiered_opt(opts);
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
    default:
        return (1);
    }
    return (0);
}

/*
 * testutil_parse_opts --
 *     Parse command line options for a test case.
 */
int
testutil_parse_opts(int argc, char *const *argv, TEST_OPTS *opts)
{
    int ch;
    static const char *getopt_args = "A:b:dh:n:o:P:pR:T:t:vW:";

    testutil_parse_begin_opt(argc, argv, getopt_args, opts);
    while ((ch = __wt_getopt(opts->progname, opts->argc, opts->argv, getopt_args)) != EOF)
        if (testutil_parse_single_opt(opts, ch) != 0) {
            (void)fprintf(stderr, "usage: %s%s\n", opts->progname, opts->usage);
            return (1);
        }

    testutil_parse_end_opt(opts);
    return (0);
}
