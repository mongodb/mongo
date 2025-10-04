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
 * JIRA ticket reference: WT-2695 Test case description: Smoke-test the CRC.
 */

/*
 * check --
 *     Error if two given checksum values do not match to one another.
 */
static inline void
check(uint32_t checksum1, uint32_t checksum2, size_t len, const char *msg)
{
    testutil_assertfmt(checksum1 == checksum2,
      "%s checksum mismatch of %" WT_SIZET_FMT " bytes: %#08x != %#08x\n", msg, len, checksum1,
      checksum2);
}

#define DATASIZE (128 * 1024)

/*
 * cumulative_checksum --
 *     Return the checksum of the data calculated cumulatively over the chunk sizes.
 */
static inline uint32_t
cumulative_checksum(uint32_t (*checksum_seed_fn)(uint32_t, const void *, size_t), size_t chunk_len,
  uint8_t *data, size_t len)
{
    uint64_t chunks, i;
    uint32_t checksum;

    testutil_assert(chunk_len <= len && chunk_len != 0);
    for (i = 0, checksum = 0, chunks = len / chunk_len; i < chunks; i++)
        checksum = checksum_seed_fn(checksum, data + (i * chunk_len), chunk_len);
    /* The last remaining bytes, less than a chunk. */
    if (len - (i * chunk_len) != 0)
        checksum = checksum_seed_fn(checksum, data + (i * chunk_len), len - (i * chunk_len));

    return (checksum);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_RAND_STATE rnd;
    size_t chunk_len, len;
    uint32_t (*hw_checksum_seed_fn)(uint32_t, const void *, size_t);
    uint32_t cumulative_hw, cumulative_sw;
    uint32_t hw, sw;
    uint8_t *data;
    uint8_t data_ff[32];
    u_int i, j, k, length, misalignment;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_recreate_dir(opts->home);
    testutil_check(wiredtiger_open(opts->home, NULL,
      "create,statistics=(all),statistics_log=(json,on_close,wait=1)", &opts->conn));

    /* Initialize the RNG. */
    __wt_random_init(NULL, &rnd);

    /* Allocate aligned memory for the data. */
    data = dcalloc(DATASIZE, sizeof(uint8_t));
    memset(data_ff, 0xff, sizeof(data_ff));

    /* When available, get the hardware checksum function that accepts a starting seed. */
    hw_checksum_seed_fn = wiredtiger_crc32c_with_seed_func();

    /*
     * Some simple known checksums.
     */
    len = 1;
    hw = __wt_checksum(data, len);
    check(hw, (uint32_t)0x527d5351, len, "nul x1: hardware");
    sw = __wt_checksum_sw(data, len);
    check(sw, (uint32_t)0x527d5351, len, "nul x1: software");

    len = 2;
    hw = __wt_checksum(data, len);
    check(hw, (uint32_t)0xf16177d2, len, "nul x2: hardware");
    sw = __wt_checksum_sw(data, len);
    check(sw, (uint32_t)0xf16177d2, len, "nul x2: software");
    cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, 1, data, len);
    check(cumulative_hw, (uint32_t)0xf16177d2, len, "(cumulative calculation) nul x2: hardware");
#if !defined(__s390x__)
    /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
    cumulative_sw = cumulative_checksum(__wt_checksum_with_seed_sw, 1, data, len);
    check(cumulative_sw, (uint32_t)0xf16177d2, len, "(cumulative calculation) nul x2: software");
#endif

    len = 3;
    hw = __wt_checksum(data, len);
    check(hw, (uint32_t)0x6064a37a, len, "nul x3: hardware");
    sw = __wt_checksum_sw(data, len);
    check(sw, (uint32_t)0x6064a37a, len, "nul x3: software");
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, chunk_len, data, len);
        check(
          cumulative_hw, (uint32_t)0x6064a37a, len, "(cumulative calculation) nul x3: hardware");
    }
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_sw = cumulative_checksum(__wt_checksum_with_seed_sw, chunk_len, data, len);
#if !defined(__s390x__)
        /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
        check(
          cumulative_sw, (uint32_t)0x6064a37a, len, "(cumulative calculation) nul x3: software");
#endif
    }

    len = 4;
    hw = __wt_checksum(data, len);
    check(hw, (uint32_t)0x48674bc7, len, "nul x4: hardware");
    sw = __wt_checksum_sw(data, len);
    check(sw, (uint32_t)0x48674bc7, len, "nul x4: software");
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, chunk_len, data, len);
        check(
          cumulative_hw, (uint32_t)0x48674bc7, len, "(cumulative calculation) nul x4: hardware");
    }
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_sw = cumulative_checksum(__wt_checksum_with_seed_sw, chunk_len, data, len);
#if !defined(__s390x__)
        /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
        check(
          cumulative_sw, (uint32_t)0x48674bc7, len, "(cumulative calculation) nul x4: software");
#endif
    }

    len = 1;
    hw = __wt_checksum(data_ff, len);
    check(hw, (uint32_t)0xff000000, len, "0xff x1: hardware");
    sw = __wt_checksum_sw(data_ff, len);
    check(sw, (uint32_t)0xff000000, len, "0xff x1: software");

    len = 2;
    hw = __wt_checksum(data_ff, len);
    check(hw, (uint32_t)0xffff0000, len, "0xff x2: hardware");
    sw = __wt_checksum_sw(data_ff, len);
    check(sw, (uint32_t)0xffff0000, len, "0xff x2: software");

    len = 3;
    hw = __wt_checksum(data_ff, len);
    check(hw, (uint32_t)0xffffff00, len, "0xff x3: hardware");
    sw = __wt_checksum_sw(data_ff, len);
    check(sw, (uint32_t)0xffffff00, len, "0xff x3: software");

    len = 4;
    hw = __wt_checksum(data_ff, len);
    check(hw, (uint32_t)0xffffffff, len, "0xff x4: hardware");
    sw = __wt_checksum_sw(data_ff, len);
    check(sw, (uint32_t)0xffffffff, len, "0xff x4: software");

    len = 5;
    hw = __wt_checksum(data_ff, len);
    check(hw, (uint32_t)0x5282acae, len, "0xff x5: hardware");
    sw = __wt_checksum_sw(data_ff, len);
    check(sw, (uint32_t)0x5282acae, len, "0xff x5: software");

    len = 9;
    hw = __wt_checksum(data_ff, len);
    check(hw, (uint32_t)0xe80f2564, len, "0xff x9: hardware");
    sw = __wt_checksum_sw(data_ff, len);
    check(sw, (uint32_t)0xe80f2564, len, "0xff x9: software");
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, chunk_len, data_ff, len);
        check(
          cumulative_hw, (uint32_t)0xe80f2564, len, "(cumulative calculation) 0xff x9: hardware");
    }
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_sw = cumulative_checksum(__wt_checksum_with_seed_sw, chunk_len, data_ff, len);
#if !defined(__s390x__)
        /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
        check(
          cumulative_sw, (uint32_t)0xe80f2564, len, "(cumulative calculation) 0xff x9: software");
#endif
    }

    len = strlen("123456789");
    memcpy(data, "123456789", len);
    hw = __wt_checksum(data, len);
    check(hw, (uint32_t)0xe3069283, len, "known string #1: hardware");
    sw = __wt_checksum_sw(data, len);
    check(sw, (uint32_t)0xe3069283, len, "known string #1: software");
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, chunk_len, data, len);
        check(cumulative_hw, (uint32_t)0xe3069283, len,
          "(cumulative calculation) known string #1: hardware");
    }
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_sw = cumulative_checksum(__wt_checksum_with_seed_sw, chunk_len, data, len);
#if !defined(__s390x__)
        /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
        check(cumulative_sw, (uint32_t)0xe3069283, len,
          "(cumulative calculation) known string #1: software");
#endif
    }

    len = strlen("The quick brown fox jumps over the lazy dog");
    memcpy(data, "The quick brown fox jumps over the lazy dog", len);
    hw = __wt_checksum(data, len);
    check(hw, (uint32_t)0x22620404, len, "known string #2: hardware");
    sw = __wt_checksum_sw(data, len);
    check(sw, (uint32_t)0x22620404, len, "known string #2: software");
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, chunk_len, data, len);
        check(cumulative_hw, (uint32_t)0x22620404, len,
          "(cumulative calculation) known string #2: hardware");
    }
    for (chunk_len = 1; chunk_len < len; chunk_len++) {
        cumulative_sw = cumulative_checksum(__wt_checksum_with_seed_sw, chunk_len, data, len);
#if !defined(__s390x__)
        /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
        check(cumulative_sw, (uint32_t)0x22620404, len,
          "(cumulative calculation) known string #2: software");
#endif
    }

    /*
     * Offset the string by 1 to ensure the hardware code handles unaligned reads.
     */
    hw = __wt_checksum(data + 1, len - 1);
    check(hw, (uint32_t)0xae11f7f5, len, "known string #2: hardware");
    sw = __wt_checksum_sw(data + 1, len - 1);
    check(sw, (uint32_t)0xae11f7f5, len, "known string #2: software");
    for (chunk_len = 1; chunk_len < len - 1; chunk_len++) {
        cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, chunk_len, data + 1, len - 1);
        check(cumulative_hw, (uint32_t)0xae11f7f5, len,
          "(cumulative calculation) known string #2: hardware");
    }
    for (chunk_len = 1; chunk_len < len - 1; chunk_len++) {
        cumulative_sw =
          cumulative_checksum(__wt_checksum_with_seed_sw, chunk_len, data + 1, len - 1);
#if !defined(__s390x__)
        /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
        check(cumulative_sw, (uint32_t)0xae11f7f5, len,
          "(cumulative calculation) known string #2: software");
#endif
    }

    /*
     * Checksums of power-of-two data chunks.
     */
    for (i = 0, len = 512; i < WT_THOUSAND; ++i) {
        for (j = 0; j < len; ++j)
            data[j] = __wt_random(&rnd) & 0xff;
        hw = __wt_checksum(data, len);
        sw = __wt_checksum_sw(data, len);
        check(hw, sw, len, "random power-of-two");

        /* Check cumulative checksum over the chunks of random size. Do this a few times. */
        for (k = 0; k < 10; k++) {
            chunk_len = (__wt_random(&rnd) % len) + 1; /* Avoid 0 sized chunks. */
            cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, chunk_len, data, len);
            cumulative_sw = cumulative_checksum(__wt_checksum_with_seed_sw, chunk_len, data, len);
            check(cumulative_hw, hw, len, "(cumulative calculation) random power-of-two: hardware");
#if !defined(__s390x__)
            /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
            check(cumulative_sw, sw, len, "(cumulative calculation) random power-of-two: software");
#endif
        }

        len *= 2;
        if (len > DATASIZE || len == 0)
            len = 512;
    }

    /*
     * Checksums of random data chunks.
     */
    for (i = 0; i < WT_THOUSAND; ++i) {
        do {
            len = __wt_random(&rnd) % DATASIZE;
        } while (len == 0);
        for (j = 0; j < len; ++j)
            data[j] = __wt_random(&rnd) & 0xff;
        hw = __wt_checksum(data, len);
        sw = __wt_checksum_sw(data, len);
        check(hw, sw, len, "random");

        /* Check cumulative checksum over the chunks of random size. Do this a few times. */
        for (k = 0; k < 10; k++) {
            chunk_len = (__wt_random(&rnd) % len) + 1; /* Avoid 0 sized chunks. */
            cumulative_hw = cumulative_checksum(hw_checksum_seed_fn, chunk_len, data, len);
            cumulative_sw = cumulative_checksum(__wt_checksum_with_seed_sw, chunk_len, data, len);
            check(cumulative_hw, hw, len, "(cumulative calculation) random: hardware");
#if !defined(__s390x__)
            /* FIXME-WT-12067: Re-enable after fixing CRC with seed in software on s390x. */
            check(cumulative_sw, sw, len, "(cumulative calculation) random: software");
#endif
        }
    }

    /*
     * "Strobed" misalignments - test every combo of size/misalignment up to 16B.
     */
    for (length = 0; length < 16; length++) {
        for (misalignment = 0; misalignment < 16; misalignment++) {
            hw = __wt_checksum(&data_ff[misalignment], length);
            sw = __wt_checksum_sw(&data_ff[misalignment], length);
            check(hw, sw, length, "0xff: strobed");
        }
    }

#if defined(__s390x__)
    WT_UNUSED(cumulative_sw);
#endif

    free(data);
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
