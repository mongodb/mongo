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

/*
 * This is a unit test for the testutil_parse_opts and testutil_parse_single_opt functions.
 *
 * In one part of the test, we do a straightforward test of the testutil_parse_opts function. This
 * function is used by test programs to parse a fixed set of commonly used options, for example
 * ".... -h home_dir -T 3 -v" sets the home directory as indicated, the thread count to 3 and turns
 * on verbose. Options are parsed and a TEST_OPTS structure is modified accordingly. To test this,
 * we have a set of simulated command lines and our expected contents of the TEST_OPTS structure.
 * These command lines and expected TEST_OPTS appear as the first entries in the driver array below.
 *
 * In the second part of the test, we want to parse additional options. Many test programs need some
 * options from the fixed set provided by the testutil_parse_opts function, but perhaps need
 * additional options. We have in mind a fictional test program that wants to use the standard
 * testutil parsing for options 'b', 'P', 'T', and 'v', and has added its own options: 'c', 'd',
 * 'e', and 'f', which it stores in a struct below called FICTIONAL_OPTS. Note that its 'd' option
 * overrides the standard 'd' option. To do this sort of "extended" parsing, we need to use a
 * particular idiom that uses testutil_parse_begin_opt, getopt, testutil_parse_single_opt, and
 * testutil_parse_end_opt, as seen in the check function.
 */

extern char *__wt_optarg; /* option argument */
extern int __wt_optind;   /* argv position, needed to reset __wt_getopt parser */
extern int __wt_optreset; /* needed to reset the parser internal state */

/*
 * This structure aids in testing testutil_parse_single_opt. The options here only have meaning to
 * our fictional test program.
 */
typedef struct {
    char *checkpoint_name; /* -c option, takes a string argument */
    bool delete_flag;      /* -d option */
    bool energize_flag;    /* -e option, it's fictional! */
    int fuzziness_option;  /* -f option, takes an int argument */
} FICTIONAL_OPTS;

/*
 * The values in this structure map directly to the values in TEST_OPTS that we expect to be
 * modified by this program.
 */
typedef struct {
    const char *build_dir;             /* Build directory path */
    const char *tiered_storage_source; /* Tiered storage source */
    uint64_t data_seed;                /* Random seed for data ops */
    uint64_t extra_seed;               /* Random seed for read ops */
    bool tiered_storage;               /* Configure tiered storage */
    bool verbose;                      /* Run in verbose mode */
    uint64_t nthreads;                 /* Number of threads */
} SUBSET_TEST_OPTS;

/*
 * This is an indicator that we will expect any value other than zero.
 */
#define NONZERO 0xfafafafa

/*
 * This drives the testing. For each given command_line, there is a matching expected TEST_OPTS
 * structure. When argv[0] is "parse_opts", we are driving testutil_parse_opts. If argv[0] is
 * "parse_single_opt", then we are parsing some of our "own" options, put into an FICTIONAL_OPTS
 * struct, and using testutil_parse_single_opt to parse any we don't recognize, those are put into
 * TEST_OPTS.
 */
typedef struct {
    const char *command_line[10];
    SUBSET_TEST_OPTS subset_expected;
    FICTIONAL_OPTS fiction_expected;
} TEST_DRIVER;

/*
 * We've chosen tests to cover:
 *  - string options both appearing as "-b option" and "-boption"
 *  - int options also appearing both as "-T 21" and "-T21"
 *  - tiered storage multiple character options starting with "-P", like "-PT" and "-Po name".
 *  - flag options like "-v".
 *  - our set of "fictional" arguments.
 *
 */
static TEST_DRIVER driver[] = {
  {{"parse_opts", "-b", "builddir", "-T", "21", NULL},
    {"builddir", NULL, NONZERO, NONZERO, false, false, 21}, {NULL, 0, 0, 0}},

  {{"parse_opts", "-bbuilddir", "-T21", NULL},
    {"builddir", NULL, NONZERO, NONZERO, false, false, 21}, {NULL, 0, 0, 0}},

  {{"parse_opts", "-T21", NULL}, {NULL, NULL, NONZERO, NONZERO, false, false, 21}, {NULL, 0, 0, 0}},
  /*
   * If -PT is used, the tiered_storage source is set to dir_store, even if -Po is not used. Also
   * when -PT is used, random seeds are initialized to some non-zero value.
   */
  {{"parse_opts", "-v", "-PT", NULL}, {NULL, "dir_store", NONZERO, NONZERO, true, true, 0},
    {NULL, 0, 0, 0}},

  {{"parse_opts", "-v", "-Po", "dir_store", "-PT", NULL},
    {NULL, "dir_store", NONZERO, NONZERO, true, true, 0}, {NULL, 0, 0, 0}},

  {{"parse_opts", "-vPodir_store", "-PT", NULL},
    {NULL, "dir_store", NONZERO, NONZERO, true, true, 0}, {NULL, 0, 0, 0}},

  /* Setting random seeds can be done together or separately. */
  {{"parse_opts", "-PT", "-PSE2345,D1234", NULL}, {NULL, "dir_store", 1234, 2345, true, false, 0},
    {NULL, 0, 0, 0}},

  {{"parse_opts", "-PT", "-PSD1234", "-PSE2345", NULL},
    {NULL, "dir_store", 1234, 2345, true, false, 0}, {NULL, 0, 0, 0}},

  {{"parse_opts", "-PT", "-PSD1234", NULL}, {NULL, "dir_store", 1234, NONZERO, true, false, 0},
    {NULL, 0, 0, 0}},

  /*
   * From here on, we are using some "extended" options, see previous comment. We set the argv[0] to
   * "parse_single_opt" to indicate to use the extended parsing idiom.
   */
  {{"parse_single_opt", "-vd", "-Podir_store", "-c", "string_opt", "-PT", NULL},
    {NULL, "dir_store", NONZERO, NONZERO, true, true, 0}, {(char *)"string_opt", true, false, 0}},

  {{"parse_single_opt", "-dv", "-Podir_store", "-cstring_opt", "-PT", NULL},
    {NULL, "dir_store", NONZERO, NONZERO, true, true, 0}, {(char *)"string_opt", true, false, 0}},

  {{"parse_single_opt", "-ev", "-cstring_opt", "-Podir_store", "-PT", "-f", "22", NULL},
    {NULL, "dir_store", NONZERO, NONZERO, true, true, 0}, {(char *)"string_opt", false, true, 22}},

  {{"parse_single_opt", "-evd", "-Podir_store", "-PT", "-f22", NULL},
    {NULL, "dir_store", NONZERO, NONZERO, true, true, 0}, {NULL, true, true, 22}},

  {{"parse_single_opt", "-v", "-Podir_store", "-PT", NULL},
    {NULL, "dir_store", NONZERO, NONZERO, true, true, 0}, {NULL, false, false, 0}},
};

/*
 * report --
 *     Show any changed fields in the options.
 */
static void
report(const TEST_OPTS *opts, FICTIONAL_OPTS *fiction_opts)
{
#define REPORT_INT(o, field)                                      \
    do {                                                          \
        if (o->field != 0)                                        \
            printf(#field ": %" PRIu64 "\n", (uint64_t)o->field); \
    } while (0)
#define REPORT_STR(o, field)                   \
    do {                                       \
        if (o->field != NULL)                  \
            printf(#field ": %s\n", o->field); \
    } while (0)

    REPORT_STR(opts, home);
    REPORT_STR(opts, build_dir);
    REPORT_STR(opts, tiered_storage_source);
    REPORT_INT(opts, table_type);
    REPORT_INT(opts, data_seed);
    REPORT_INT(opts, extra_seed);
    REPORT_INT(opts, do_data_ops);
    REPORT_INT(opts, preserve);
    REPORT_INT(opts, tiered_storage);
    REPORT_INT(opts, verbose);
    REPORT_INT(opts, nrecords);
    REPORT_INT(opts, nops);
    REPORT_INT(opts, nthreads);
    REPORT_INT(opts, n_append_threads);
    REPORT_INT(opts, n_read_threads);
    REPORT_INT(opts, n_write_threads);
    REPORT_STR(fiction_opts, checkpoint_name);
    REPORT_INT(fiction_opts, delete_flag);
    REPORT_INT(fiction_opts, energize_flag);
    REPORT_INT(fiction_opts, fuzziness_option);
    printf("Seeds: " TESTUTIL_SEED_FORMAT "\n", opts->data_seed, opts->extra_seed);
}

/*
 * check --
 *     Call testutil_parse_opts or use extended parsing with testutil_parse_single_opt and return
 *     options.
 */
static void
check(int argc, char *const *argv, TEST_OPTS *opts, FICTIONAL_OPTS *fiction_opts)
{
    int ch;
    const char *fiction_usage = " [-c string] [-d] [-e] [-f int]";
    const char *prog;

    memset(opts, 0, sizeof(*opts));
    memset(fiction_opts, 0, sizeof(*fiction_opts));

    /* This may be called multiple times, so reset the __wt_getopt parser. */
    __wt_optind = 1;
    __wt_optreset = 1;

    prog = argv[0];
    if (strchr(prog, '/') != NULL)
        prog = strchr(prog, '/') + 1;

    if (strcmp(prog, "parse_opts") == 0) {
        /* Regular test of testutil_parse_opts, using only the options that it provides. */

        testutil_check(testutil_parse_opts(argc, argv, opts));
    } else {
        /*
         * Test of extended parsing, in which we'll parse some options that we know about and rely
         * on testutil_parse_single_opt to cover the options it knows about.
         */
        testutil_assert(strcmp(prog, "parse_single_opt") == 0);

        /*
         * For this part of the testing, we're parsing options for a fictional test program. This
         * test program wants to have the standard testutil parsing for options 'b', 'P', 'T', and
         * 'v', and has added its own options: 'c', 'd', 'e', and 'f'. We use the following idiom to
         * accomplish this.
         */

        /* "b:P:T:v" are the only options we want testutil to handle. */
        testutil_parse_begin_opt(argc, argv, "b:P:T:v", opts);

        /* We list the entire set of options we want to support when we call getopt. */
        while ((ch = __wt_getopt(opts->progname, argc, argv, "b:c:def:P:T:v")) != EOF)
            switch (ch) {
            case 'c':
                fiction_opts->checkpoint_name = __wt_optarg;
                break;
            case 'd':
                fiction_opts->delete_flag = true;
                break;
            case 'e':
                fiction_opts->energize_flag = true;
                break;
            case 'f':
                fiction_opts->fuzziness_option = atoi(__wt_optarg);
                break;
            default:
                /* The option is either one that we're asking testutil to support, or illegal. */
                if (testutil_parse_single_opt(opts, ch) != 0) {
                    (void)fprintf(
                      stderr, "usage: %s%s%s\n", opts->progname, fiction_usage, opts->usage);
                    testutil_assert(false);
                }
            }
        /*
         * We are finished parsing, so ask testutil to finish any extra processing of the options.
         */
        testutil_parse_end_opt(opts);
    }
}

/*
 * verify_expect --
 *     Verify the returned options against the expected options.
 */
static void
verify_expect(
  TEST_OPTS *opts, FICTIONAL_OPTS *fiction_opts, TEST_OPTS *expect, FICTIONAL_OPTS *fiction_expect)
{
#define VERIFY_INT(o, e, field)                    \
    do {                                           \
        if (o->field != 0 || e->field != 0)        \
            testutil_assert(o->field == e->field); \
    } while (0)
#define VERIFY_STR(o, e, field)                               \
    do {                                                      \
        if (o->field != NULL || e->field != NULL) {           \
            testutil_assert(o->field != NULL);                \
            testutil_assert(e->field != NULL);                \
            testutil_assert(strcmp(o->field, e->field) == 0); \
        }                                                     \
    } while (0)
/*
 * Random seeds are treated specially. If we've marked the expected value to be NONZERO, that's all
 * we need to confirm.
 */
#define VERIFY_RANDOM_INT(o, e, field)      \
    do {                                    \
        if (e->field == NONZERO)            \
            testutil_assert(o->field != 0); \
        else                                \
            VERIFY_INT(o, e, field);        \
    } while (0)

    /*
     * opts->home is always set, even without -h on the command line, so don't check it here. If
     * tiered_storage is set then build_dir is deduced from the test program.
     */
    if (opts->tiered_storage != true)
        VERIFY_STR(opts, expect, build_dir);
    VERIFY_STR(opts, expect, tiered_storage_source);
    VERIFY_INT(opts, expect, table_type);
    VERIFY_RANDOM_INT(opts, expect, data_seed);
    VERIFY_RANDOM_INT(opts, expect, extra_seed);
    VERIFY_INT(opts, expect, do_data_ops);
    VERIFY_INT(opts, expect, preserve);
    VERIFY_INT(opts, expect, tiered_storage);
    VERIFY_INT(opts, expect, verbose);
    VERIFY_INT(opts, expect, nrecords);
    VERIFY_INT(opts, expect, nops);
    VERIFY_INT(opts, expect, nthreads);
    VERIFY_INT(opts, expect, n_append_threads);
    VERIFY_INT(opts, expect, n_read_threads);
    VERIFY_INT(opts, expect, n_write_threads);

    VERIFY_STR(fiction_opts, fiction_expect, checkpoint_name);
    VERIFY_INT(fiction_opts, fiction_expect, delete_flag);
    VERIFY_INT(fiction_opts, fiction_expect, energize_flag);
    VERIFY_INT(fiction_opts, fiction_expect, fuzziness_option);
}

/*
 * cleanup --
 *     Clean up allocated resources.
 */
static void
cleanup(TEST_OPTS *opts, FICTIONAL_OPTS *fiction_opts)
{
    (void)fiction_opts; /* Nothing to clean up here. */

    testutil_cleanup(opts);
}

/*
 * main --
 *     Unit test for test utility functions.
 */
int
main(int argc, char *argv[])
{
    FICTIONAL_OPTS *fiction_expect, fiction_opts;
    SUBSET_TEST_OPTS *subset;
    TEST_OPTS expect, opts;
    size_t i;
    int nargs;
    char *const *cmd;

    if (argc > 1) {
        /*
         * The first arg must be --parse_opt or --parse_single_opt, we'll make argv[0] point to
         * "parse_opts" or "parse_single_opt" so our "check" parser knows what kind of parsing to be
         * done. This path is not used by test scripts, but can be useful for manual testing.
         */
        if (strcmp(argv[1], "--parse_opts") != 0 && strcmp(argv[1], "--parse_single_opt") != 0) {
            fprintf(stderr,
              "Error: test_wt9937_parse_opts first argument must be --parse_opts or "
              "--parse_single_opt, remaining options will be parsed accordingly\n");
            exit(EXIT_FAILURE);
        }

        argc--;
        argv++;
        argv[0] += 2; /* skip past -- */

        check(argc, argv, &opts, &fiction_opts);
        report(&opts, &fiction_opts);
        cleanup(&opts, &fiction_opts);
    } else {
        /*
         * For normal testing, we run the whole test, parsing each command line from the driver
         * array.
         */
        for (i = 0; i < WT_ELEMENTS(driver); i++) {
            cmd = (char *const *)driver[i].command_line;
            for (nargs = 0; cmd[nargs] != NULL; nargs++)
                ;

            /*
             * Fill our expected test utility options array with only the subset of values we are
             * expecting to be modified. We expect all the other values to be zeroed or NULL.
             */
            subset = &driver[i].subset_expected;
            WT_CLEAR(expect);
            expect.build_dir = (char *)subset->build_dir;
            expect.tiered_storage_source = (char *)subset->tiered_storage_source;
            expect.tiered_storage = subset->tiered_storage;
            expect.verbose = subset->verbose;
            expect.nthreads = subset->nthreads;
            expect.data_seed = subset->data_seed;
            expect.extra_seed = subset->extra_seed;

            fiction_expect = &driver[i].fiction_expected;
            check(nargs, cmd, &opts, &fiction_opts);

            verify_expect(&opts, &fiction_opts, &expect, fiction_expect);
            cleanup(&opts, &fiction_opts);
        }
    }

    exit(0);
}
