/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

struct __wt_bloom {
	const char *uri;
	char *config;
	uint8_t *bitstring;     /* For in memory representation. */
	WT_SESSION_IMPL *session;
	WT_CURSOR *c;

	uint32_t k;		/* The number of hash functions used. */
	uint32_t factor;	/* The number of bits per item inserted. */
	uint64_t m;		/* The number of slots in the bit string. */
	uint64_t n;		/* The number of items to be inserted. */
};

struct __wt_bloom_hash {
	uint64_t h1, h2;	/* The two hashes used to calculate bits. */
};
