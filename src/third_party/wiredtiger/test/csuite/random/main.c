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

extern int __wt_optind;

static const uint32_t EXPECTED_RANDOM[] = {545736098, 2010324742, 2686179461, 3017381033, 410404515,
  3042610605, 893387793, 44244647, 89435757, 1127483053, 1854156410, 2560384123, 3010064238,
  1301488617, 3323393529, 2434747831, 3994507216, 1580311051, 2696652026, 2641292453, 1288576349,
  2355051412, 4276315443, 1777047127, 3932279793, 3621597994, 926735067, 2456119193, 2375585859,
  401207175, 2174557645, 311488597, 1590435109, 1836552166, 174471706};

/*
 * test_random --
 *     Verify that the random number generator produces the expected output.
 */
static void
test_random(bool verbose)
{
    WT_RAND_STATE rnd;
    uint64_t count;
    uint32_t r;
    int i;

    i = 0;
    count = 0;

    __wt_random_init(&rnd);

    if (verbose)
        printf("%2s  %11s  %10s\n", "#", "count", "random");
    for (;;) {
        count++;
        r = __wt_random(&rnd);

        if (count == ((uint64_t)1) << i) {
            if (verbose)
                printf("%2d  %11" PRIu64 "  %10" PRIu32 "\n", i, count, r);

            testutil_assert(r == EXPECTED_RANDOM[i]);

            i++;
            if ((size_t)i >= sizeof(EXPECTED_RANDOM) / sizeof(EXPECTED_RANDOM[0]))
                break;
        }
    }
}

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * usage --
 *     Print the usage information.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-v]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * main --
 *     The main function.
 */
int
main(int argc, char *argv[])
{
    int ch;
    bool verbose;

    (void)testutil_set_progname(argv);

#ifdef ENABLE_ANTITHESIS
    if (argv != NULL) { /* Prevent the compiler from complaining about dead code below.*/
        printf("This test is not compatible with Antithesis.\n");
        return (EXIT_SUCCESS);
    }
#endif

    verbose = false;

    while ((ch = __wt_getopt(progname, argc, argv, "v")) != EOF)
        switch (ch) {
        case 'v':
            verbose = true;
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    test_random(verbose);
    return (EXIT_SUCCESS);
}
