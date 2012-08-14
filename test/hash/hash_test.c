/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "hash.h"

#include <assert.h>

/* Main entry points for the test programs. */
extern int city_hash_main();
extern int test_fnv64(Fnv64_t init_hval, Fnv64_t mask, int v_flag, int code);

int  usage(void);

const char *progname;				/* Program name */

typedef enum {
	TEST_ALL=0,
	TEST_FNV,
	TEST_CITY
} HASH_TEST_TYPE;

int
main(int argc, char *argv[])
{
	int ch, r, ret, verbose;
	HASH_TEST_TYPE tt;
	Fnv64_t bmask;		/* mask to apply to FNV output */

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	tt = TEST_ALL;
	r = 0;
	verbose = 0;
	while ((ch = getopt(argc, argv, "cfv")) != EOF)
		switch (ch) {
		case 'c':
			tt = TEST_CITY;
			break;
		case 'f':
			tt = TEST_FNV;
			break;
		case 'v':
			verbose = 1;
			break;
		case '?':
		default:
			return (usage());
		}

	argc -= optind;
	argv += optind;
	if (argc != 0)
		return (usage());

	printf("hash test run started\n");

	/*
	 * FNV test
	 */
	if (tt == TEST_FNV || tt == TEST_ALL) {
		if (verbose)
			printf("Running FNV hash test\n");
		bmask = (Fnv64_t)0xffffffffffffffffULL;
		ret = test_fnv64(FNV1A_64_INIT, bmask, verbose, 1);

		if (ret != 0) {
			printf("failed vector (1 is 1st test): %d\n", ret);
			exit(EXIT_FAILURE);
		} else if (verbose)
			printf("Finished FNV hash test\n");
	}

	/*
	 * City hash test
	 */
	if (tt == TEST_CITY || tt == TEST_ALL) {
		if (verbose)
			printf("Running City hash test\n");
		if ((ret = city_hash_main()) != 0) {
			printf("Failed city hash tests\n");
			return (EXIT_FAILURE);
		}
		if (verbose)
			printf("Finished city hash test\n");
	}

	printf("hash test run completed\n");
	return (EXIT_SUCCESS);
}

int
usage(void)
{
	(void)fprintf(stderr,
			"usage: %s\n", progname);
	return (EXIT_FAILURE);
}
