/*-
 * Public Domain 2014-2016 MongoDB, Inc.
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
 * JIRA ticket reference: WT-2695
 * Test case description: Smoke-test the CRC.
 */

void (*custom_die)(void) = NULL;

uint32_t cksum_sw(const void *, size_t);

static inline void
check(uint32_t hw, uint32_t sw, size_t len, const char *msg)
{
	testutil_checkfmt(hw == sw ? 0 : 1,
	    "%s checksum mismatch of %" WT_SIZET_FMT " bytes: %#08x != %#08x\n",
	    msg, len, hw, sw);
}

#define	DATASIZE	(128 * 1024)
int
main(void)
{
	WT_RAND_STATE rnd;
	size_t len;
	uint32_t hw, sw;
	u_int i, j;
	uint8_t *data;

	/* Allocate aligned memory for the data. */
	data = dcalloc(DATASIZE, sizeof(uint8_t));

	/* Initialize the RNG. */
	testutil_check(__wt_random_init_seed(NULL, &rnd));

	/* Initialize the WiredTiger library checksum functions. */
	__wt_cksum_init();

	/*
	 * Some simple known checksums.
	 */
	len = 1;
	hw = __wt_cksum(data, len);
	check(hw, (uint32_t)0x527d5351, len, "nul x1: hardware");
	sw = cksum_sw(data, len);
	check(sw, (uint32_t)0x527d5351, len, "nul x1: software");

	len = 2;
	hw = __wt_cksum(data, len);
	check(hw, (uint32_t)0xf16177d2, len, "nul x2: hardware");
	sw = cksum_sw(data, len);
	check(sw, (uint32_t)0xf16177d2, len, "nul x2: software");

	len = 3;
	hw = __wt_cksum(data, len);
	check(hw, (uint32_t)0x6064a37a, len, "nul x3: hardware");
	sw = cksum_sw(data, len);
	check(sw, (uint32_t)0x6064a37a, len, "nul x3: software");

	len = 4;
	hw = __wt_cksum(data, len);
	check(hw, (uint32_t)0x48674bc7, len, "nul x4: hardware");
	sw = cksum_sw(data, len);
	check(sw, (uint32_t)0x48674bc7, len, "nul x4: software");

	len = strlen("123456789");
	memcpy(data, "123456789", len);
	hw = __wt_cksum(data, len);
	check(hw, (uint32_t)0xe3069283, len, "known string #1: hardware");
	sw = cksum_sw(data, len);
	check(sw, (uint32_t)0xe3069283, len, "known string #1: software");

	len = strlen("The quick brown fox jumps over the lazy dog");
	memcpy(data, "The quick brown fox jumps over the lazy dog", len);
	hw = __wt_cksum(data, len);
	check(hw, (uint32_t)0x22620404, len, "known string #2: hardware");
	sw = cksum_sw(data, len);
	check(sw, (uint32_t)0x22620404, len, "known string #2: software");

	/*
	 * Checksums of power-of-two data chunks.
	 */
	for (i = 0, len = 512; i < 1000; ++i) {
		for (j = 0; j < len; ++j)
			data[j] = __wt_random(&rnd) & 0xff;
		hw = __wt_cksum(data, len);
		sw = cksum_sw(data, len);
		check(hw, sw, len, "random power-of-two");

		len *= 2;
		if (len > DATASIZE)
			len = 512;
	}

	/*
	 * Checksums of random data chunks.
	 */
	for (i = 0; i < 1000; ++i) {
		len = __wt_random(&rnd) % DATASIZE;
		for (j = 0; j < len; ++j)
			data[j] = __wt_random(&rnd) & 0xff;
		hw = __wt_cksum(data, len);
		sw = cksum_sw(data, len);
		check(hw, sw, len, "random");
	}

	free(data);
	return (EXIT_SUCCESS);
}
