/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-present WiredTiger, Inc.
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

/* Helpers for output formatting and size checks */

/*
 * print_hex_bytes --
 *     Print a buffer as space-separated hex bytes.
 */
static inline void
print_hex_bytes(const uint8_t *buf, size_t used)
{
    for (size_t k = 0; k < used; ++k)
        printf("%s%02x", k ? " " : "", buf[k]);
}

/*
 * print_bin_bytes_spaced --
 *     Print a buffer as space-separated binary bytes (MSB-first).
 */
static inline void
print_bin_bytes_spaced(const uint8_t *buf, size_t used)
{
    for (size_t k = 0; k < used; ++k) {
        putchar(' ');
        for (int b = 7; b >= 0; --b)
            putchar(((buf[k] >> b) & 1) ? '1' : '0');
    }
}

/*
 * print_hex_bin_columns --
 *     Print hex bytes and a padded binary column to align table output.
 */
static inline void
print_hex_bin_columns(const uint8_t *buf, size_t used)
{
    print_hex_bytes(buf, used);
    /* Pad to align the Bin column similarly to %-20s and %-30s headings */
    printf("%*s", (int)(21 - (used ? (3 * used - 1) : 0)), "");
    print_bin_bytes_spaced(buf, used);
}

/*
 * print_u64_array --
 *     Print an array of uint64_t values as [a, b, c].
 */
static inline void
print_u64_array(const uint64_t *arr, size_t n)
{
    printf("[");
    for (size_t j = 0; j < n; ++j)
        printf("%s%" PRIu64, j ? ", " : "", arr[j]);
    printf("]");
}

/*
 * print_hex_dump --
 *     Print a hex dump with a trailing byte count.
 */
static inline void
print_hex_dump(const uint8_t *buf, size_t used)
{
    printf("Hex dump: ");
    print_hex_bytes(buf, used);
    printf("\t(%" WT_SIZET_FMT " bytes)\n", used);
}

/*
 * print_bin_dump --
 *     Print a binary dump of a buffer, bytes separated by spaces.
 */
static inline void
print_bin_dump(const uint8_t *buf, size_t used)
{
    printf("Bin dump: ");
    for (size_t k = 0; k < used; ++k) {
        for (int b = 7; b >= 0; --b)
            putchar(((buf[k] >> b) & 1) ? '1' : '0');
        putchar(k + 1 == used ? '\n' : ' ');
    }
}

/*
 * bytes_for_values --
 *     Compute expected packed byte length for an array of values.
 */
static inline size_t
bytes_for_values(const uint64_t *vals, size_t n)
{
    size_t nibbles = 0;
    for (size_t i = 0; i < n; ++i)
        nibbles += __4b_nibbles_for_posint(vals[i]);
    return (nibbles + 1) >> 1; /* ceil(nibbles/2) */
}

/*
 * assert_bytes_for_values --
 *     Assert the packed length equals the expected byte length.
 */
static inline void
assert_bytes_for_values(size_t used, const uint64_t *vals, size_t n)
{
    size_t exp_used = bytes_for_values(vals, n);
    testutil_assert(used == exp_used);
}

/*
 * encode_array --
 *     Encode an array of positive integers into buf using 4-bit packing.
 */
static void
encode_array(const uint64_t *vals, size_t n, uint8_t *buf, size_t bufsz, size_t *usedp)
{
    WT_4B_PACK_CONTEXT ctx;
    uint8_t *p = buf;

    __4b_pack_init(&ctx, &p, buf + bufsz);
    for (size_t i = 0; i < n; ++i)
        testutil_check(__4b_pack_posint_ctx(&ctx, vals[i]));
    *usedp = (size_t)(p - buf);
}

/*
 * decode_array --
 *     Decode 'n' positive integers from buf using 4-bit packing.
 */
static void
decode_array(const uint8_t *buf, size_t len, size_t n, uint64_t *out)
{
    WT_4B_UNPACK_CONTEXT uctx;
    const uint8_t *p = buf;

    __4b_unpack_init(&uctx, &p, buf + len);
    for (size_t i = 0; i < n; ++i)
        testutil_check(__4b_unpack_posint_ctx(&uctx, &out[i]));
}

/*
 * roundtrip_and_print_pos --
 *     Encode, decode, and print a positive integer value.
 */
static void
roundtrip_and_print_pos(uint64_t val)
{
    uint8_t buf[1024];
    size_t used = 0;
    uint64_t dec = 0;
    uint64_t one[1] = {val};

    encode_array(one, 1, buf, sizeof(buf), &used);
    decode_array(buf, used, 1, &dec);
    assert_bytes_for_values(used, one, 1);

    printf("%-10" PRIu64 " ", val);
    print_hex_bin_columns(buf, used);
    printf("  %-20" PRIu64 "\n", dec);
    testutil_assert(dec == val);
}

/*
 * roundtrip_and_print_signed --
 *     Encode, decode, and print a signed integer value.
 */
static void
roundtrip_and_print_signed(int64_t sval)
{
    uint8_t buf[1024];
    uint64_t enc = __wt_encode_signed_as_positive(sval);
    uint64_t decpos = 0;
    size_t used = 0;

    encode_array(&enc, 1, buf, sizeof(buf), &used);
    decode_array(buf, used, 1, &decpos);
    assert_bytes_for_values(used, &enc, 1);

    int64_t dec = __wt_decode_positive_as_signed(decpos);
    printf("%-10" PRId64 " ", sval);
    print_hex_bin_columns(buf, used);
    printf(" %-20" PRId64 "\n", dec);
    testutil_assert(dec == sval);
}

/*
 * roundtrip_array_compact --
 *     Encode/decode an array and print compact one-line summary.
 */
static void
roundtrip_array_compact(const uint64_t *arr, size_t n)
{
    uint64_t out_local[64];
    uint8_t buf[2048];
    uint64_t *out = out_local;
    size_t used = 0;

    testutil_assert(n <= WT_ELEMENTS(out_local));
    encode_array(arr, n, buf, sizeof(buf), &used);
    assert_bytes_for_values(used, arr, n);
    decode_array(buf, used, n, out);

    print_u64_array(arr, n);
    printf(" ");
    print_u64_array(out, n);
    printf(" %" WT_SIZET_FMT "\t", used);
    print_hex_bin_columns(buf, used);
    printf("\n");

    for (size_t i = 0; i < n; ++i)
        testutil_assert(out[i] == arr[i]);
}

/*
 * roundtrip_array_multiline --
 *     Encode/decode an array and print multi-line details plus dumps.
 */
static void
roundtrip_array_multiline(const uint64_t *arr, size_t n)
{
    uint64_t out_local[64];
    uint8_t buf[2048];
    uint64_t *out = out_local;
    size_t used = 0;

    testutil_assert(n <= WT_ELEMENTS(out_local));
    encode_array(arr, n, buf, sizeof(buf), &used);
    assert_bytes_for_values(used, arr, n);
    decode_array(buf, used, n, out);

    printf("Array:    ");
    print_u64_array(arr, n);
    printf("\t(%" WT_SIZET_FMT " elements)\n", n);
    printf("Decoded:  ");
    print_u64_array(out, n);
    printf("\n");
    for (size_t i = 0; i < n; ++i)
        testutil_assert(out[i] == arr[i]);
    print_hex_dump(buf, used);
    print_bin_dump(buf, used);
}

/*
 * test_positive_integers --
 *     Exercise positive integers, including small and larger values.
 */
static void
test_positive_integers(void)
{
    /* Positive integers */
    printf("\n    Positive integers\n%-10s %-20s %-30s  %-20s\n", "Number", "Hex", "Bin",
      "Decoded Value");
    for (uint64_t i = 0; i <= 200; ++i)
        roundtrip_and_print_pos(i);

    for (int ii = 3; ii < 30; ++ii) {
        uint64_t i = 200ULL + ((uint64_t)ii * ((uint64_t)1 << ii)) / 3ULL;
        roundtrip_and_print_pos(i);
    }
}

/*
 * test_signed_integers --
 *     Exercise signed integers encoded via zigzag.
 */
static void
test_signed_integers(void)
{
    printf(
      "\n    Signed integers\n%-10s %-20s %-20s %-20s\n", "Number", "Hex", "Bin", "Decoded Value");
    for (int64_t i = -100; i <= 100; ++i)
        roundtrip_and_print_signed(i);
}

/*
 * test_pairs_of_integers --
 *     Exercise pairs of integers.
 */
static void
test_pairs_of_integers(void)
{
    printf(
      "\n    Pairs of integers\n"
      "Array\t"
      "Decoded\t"
      "Len\t"
      "Hex\t"
      "Bin\n");
    for (uint64_t i = 0; i <= 10; ++i) {
        uint64_t arr[2] = {i * (i + 1) / 2, i};
        roundtrip_array_compact(arr, 2);
    }
}

/*
 * test_small_int_arrays --
 *     Exercise arrays of small positive integers.
 */
static void
test_small_int_arrays(void)
{
    printf("\n    Array of small integers\n");
    for (uint64_t i = 1; i <= 10; ++i) {
        uint64_t arr[10];
        for (uint64_t j = 0; j < i; ++j)
            arr[j] = j;
        roundtrip_array_multiline(arr, (size_t)i);
    }
}

/*
 * test_bigger_int_arrays --
 *     Exercise arrays of bigger integers (squares).
 */
static void
test_bigger_int_arrays(void)
{
    printf("\n    Array of bigger integers\n");
    for (uint64_t i = 2; i <= 10; ++i) {
        uint64_t arr[10];
        for (uint64_t j = 0; j < i; ++j)
            arr[j] = j * j;
        roundtrip_array_multiline(arr, (size_t)i);
    }
}

/*
 * test_extreme_values --
 *     Test extremes: UINT64_MAX and zigzag edges.
 */
static void
test_extreme_values(void)
{
    uint8_t buf[128];
    size_t used = 0;
    uint64_t out = 0;

    /* UINT64_MAX */
    {
        const uint64_t v = UINT64_MAX;
        encode_array(&v, 1, buf, sizeof(buf), &used);
        assert_bytes_for_values(used, &v, 1);
        decode_array(buf, used, 1, &out);
        /* Print the value and its encoded buffer */
        printf("Extreme UINT64: %-20" PRIu64 " ", v);
        print_hex_bin_columns(buf, used);
        printf("  Decoded: %-20" PRIu64 "  Len: %" WT_SIZET_FMT "\n", out, used);
        testutil_assert(out == v);
    }

    /* Zigzag: INT64_MIN, -1, 0, 1, INT64_MAX */
    {
        int64_t svals[] = {INT64_MIN, -1, 0, 1, INT64_MAX};
        for (size_t i = 0; i < WT_ELEMENTS(svals); ++i) {
            uint64_t enc = __wt_encode_signed_as_positive(svals[i]);
            encode_array(&enc, 1, buf, sizeof(buf), &used);
            assert_bytes_for_values(used, &enc, 1);
            decode_array(buf, used, 1, &out);
            int64_t dec = __wt_decode_positive_as_signed(out);
            /* Print the signed value, encoded positive, and buffer */
            printf("Zigzag signed: %-20" PRId64 " Positive: %-20" PRIu64 " ", svals[i], enc);
            print_hex_bin_columns(buf, used);
            printf("  Decoded: %-20" PRId64 "  Len: %" WT_SIZET_FMT "\n", dec, used);
            testutil_assert(dec == svals[i]);
        }
    }
}

/*
 * test_alignment_boundaries --
 *     Test values at nibble-count boundaries and alignment flips.
 */
static void
test_alignment_boundaries(void)
{
    uint8_t buf[128];
    size_t used = 0;
    uint64_t out[8];

    /* Boundary singles */
    {
        uint64_t vals[] = {7, 8, 15, 16, 63, 64, 511, 512};
        for (size_t i = 0; i < WT_ELEMENTS(vals); ++i) {
            encode_array(&vals[i], 1, buf, sizeof(buf), &used);
            assert_bytes_for_values(used, &vals[i], 1);
            decode_array(buf, used, 1, out);
            printf("Boundary single: %-10" PRIu64 " ", vals[i]);
            print_hex_bin_columns(buf, used);
            printf("  Decoded: %-10" PRIu64 "  Len: %" WT_SIZET_FMT "\n", out[0], used);
            testutil_assert(out[0] == vals[i]);
        }
    }

    /* Alignment flip arrays */
    {
        const uint64_t a1[] = {0, 0};
        const uint64_t a2[] = {1, 1};
        const uint64_t a3[] = {7, 1};
        const uint64_t a4[] = {15, 1};
        const uint64_t a5[] = {1, 7, 1};
        const uint64_t a6[] = {7, 7, 7, 7};
        const uint64_t *arrs[] = {a1, a2, a3, a4, a5, a6};
        const size_t lens[] = {WT_ELEMENTS(a1), WT_ELEMENTS(a2), WT_ELEMENTS(a3), WT_ELEMENTS(a4),
          WT_ELEMENTS(a5), WT_ELEMENTS(a6)};
        for (size_t i = 0; i < WT_ELEMENTS(arrs); ++i) {
            encode_array(arrs[i], lens[i], buf, sizeof(buf), &used);
            assert_bytes_for_values(used, arrs[i], lens[i]);
            decode_array(buf, used, lens[i], out);
            printf("Align flip array: ");
            print_u64_array(arrs[i], lens[i]);
            printf(" ");
            print_u64_array(out, lens[i]);
            printf(" %" WT_SIZET_FMT "\t", used);
            print_hex_bin_columns(buf, used);
            printf("\n");
            for (size_t j = 0; j < lens[i]; ++j)
                testutil_assert(out[j] == arrs[i][j]);
        }
    }
}

/*
 * pack_vals_into --
 *     Helper to pack vals into a buffer with explicit end; returns ret and used.
 */
static int
pack_vals_into(const uint64_t *vals, size_t n, uint8_t *buf, size_t bufsz, size_t *usedp)
{
    WT_4B_PACK_CONTEXT ctx;
    uint8_t *p = buf;
    int ret = 0;

    __4b_pack_init(&ctx, &p, buf + bufsz);
    for (size_t i = 0; i < n; ++i) {
        ret = __4b_pack_posint_ctx(&ctx, vals[i]);
        if (ret != 0)
            break;
    }
    *usedp = (size_t)(p - buf);
    return ret;
}

/*
 * test_exact_fit_and_enomem --
 *     Verify exact-fit succeeds and size-1 fails with ENOMEM.
 */
static void
test_exact_fit_and_enomem(void)
{
    uint8_t tmp[256];
    size_t used = 0;

    const uint64_t vsets[][4] = {
      {7, 0, 0, 0},          /* small */
      {8, 0, 0, 0},          /* crosses into 2 nibbles */
      {15, 15, 15, 0},       /* mix to flip nibbles */
      {UINT64_MAX, 0, 0, 0}, /* extreme */
      {63, 64, 511, 512},    /* boundaries */
    };
    const size_t vcounts[] = {1, 1, 3, 1, 4};

    for (size_t i = 0; i < WT_ELEMENTS(vcounts); ++i) {
        const uint64_t *vals = vsets[i];
        size_t n = vcounts[i];
        size_t need = bytes_for_values(vals, n);

        /* Exact fit succeeds */
        int ret = pack_vals_into(vals, n, tmp, need, &used);
        testutil_assert(ret == 0);
        testutil_assert(used == need);
        printf("Exact-fit array: ");
        print_u64_array(vals, n);
        printf(" %" WT_SIZET_FMT "\t", used);
        print_hex_bin_columns(tmp, used);
        printf("\n");

        /* One byte short fails with ENOMEM */
        if (need > 0) {
            ret = pack_vals_into(vals, n, tmp, need - 1, &used);
            testutil_assert(ret == ENOMEM);
            printf("  (Need-1) ENOMEM as expected\n");
        }
    }
}

/*
 * try_decode_count --
 *     Decode 'count' values from buffer with explicit end; returns ret.
 */
static int
try_decode_count(const uint8_t *buf, size_t len, size_t count)
{
    WT_4B_UNPACK_CONTEXT uctx;
    const uint8_t *p = buf;
    uint64_t v;
    int ret = 0;

    __4b_unpack_init(&uctx, &p, buf + len);
    for (size_t i = 0; i < count; ++i) {
        ret = __4b_unpack_posint_ctx(&uctx, &v);
        if (ret != 0)
            return ret;
    }
    return 0;
}

/*
 * test_truncated_and_overcount_decode --
 *     Ensure truncated buffers and over-count decodes fail with EINVAL.
 */
static void
test_truncated_and_overcount_decode(void)
{
    uint8_t buf[256];
    size_t used = 0;

    /* Build buffer with several values. */
    const uint64_t vals[] = {0, 7, 8, 63, 64, 511, 512};
    encode_array(vals, WT_ELEMENTS(vals), buf, sizeof(buf), &used);

    printf("Truncated/overcount base array: ");
    print_u64_array(vals, WT_ELEMENTS(vals));
    printf(" %" WT_SIZET_FMT "\t", used);
    print_hex_bin_columns(buf, used);
    printf("\n");

    /* Truncate by one byte: should fail to decode full sequence. */
    testutil_assert(try_decode_count(buf, used - 1, WT_ELEMENTS(vals)) == EINVAL);
    printf("  Truncated by 1 -> EINVAL as expected\n");

    /* Over-count: ask for extra value beyond available: should hit EINVAL. */
    testutil_assert(try_decode_count(buf, used, WT_ELEMENTS(vals) + 1) == EINVAL);
    printf("  Over-count by 1 -> EINVAL as expected\n");

    /* Also try removing two bytes to likely split mid-number. */
    if (used > 1) {
        testutil_assert(try_decode_count(buf, used - 2, WT_ELEMENTS(vals)) == EINVAL);
        printf("  Truncated by 2 -> EINVAL as expected\n");
    }
}

/*
 * test_partial_decode_resume --
 *     Decode k values, then resume to decode the rest from the same buffer.
 */
static void
test_partial_decode_resume(void)
{
    uint8_t buf[256];
    size_t used = 0;
    const uint64_t vals[] = {0, 1, 7, 8, 15, 16, 63, 64, 255, 256};
    const size_t n = WT_ELEMENTS(vals);

    encode_array(vals, n, buf, sizeof(buf), &used);

    printf("Partial-resume array: ");
    print_u64_array(vals, n);
    printf(" %" WT_SIZET_FMT "\t", used);
    print_hex_bin_columns(buf, used);
    printf("\n");

    /* Decode first k, then resume. */
    for (size_t k = 1; k < n; ++k) {
        WT_4B_UNPACK_CONTEXT uctx;
        const uint8_t *p = buf;
        uint64_t out[WT_ELEMENTS(vals)];
        size_t i = 0;

        __4b_unpack_init(&uctx, &p, buf + used);
        for (; i < k; ++i)
            testutil_check(__4b_unpack_posint_ctx(&uctx, &out[i]));
        for (; i < n; ++i)
            testutil_check(__4b_unpack_posint_ctx(&uctx, &out[i]));
        for (i = 0; i < n; ++i)
            testutil_assert(out[i] == vals[i]);
    }
}

/*
 * rand_u64 --
 *     Helper: compose a 64-bit random from testutil_random().
 */
static inline uint64_t
rand_u64(WT_RAND_STATE *rnd)
{
    return ((uint64_t)testutil_random(rnd) << 32) | (uint64_t)testutil_random(rnd);
}

/*
 * test_random_fuzz --
 *     Fuzz random arrays of values for round-trip encode/decode.
 */
static void
test_random_fuzz(void)
{
    /* Deterministic seed for reproducibility. */
    WT_RAND_STATE rnd;
    testutil_random_from_seed(&rnd, testutil_random(NULL));

    uint8_t buf[8192];
    size_t used = 0;

    /* Positive integers fuzz */
    for (int iter = 0; iter < 2000; ++iter) {
        uint64_t vals[64], out[64];
        size_t n = (size_t)(testutil_random(&rnd) % WT_ELEMENTS(vals));

        for (size_t i = 0; i < n; ++i) {
            switch (testutil_random(&rnd) % 6) {
            case 0:
                vals[i] = (uint64_t)(testutil_random(&rnd) % 32);
                break;
            case 1:
                vals[i] = (uint64_t)(testutil_random(&rnd) % 512);
                break;
            case 2:
                vals[i] = rand_u64(&rnd);
                break;
            case 3: {
                uint32_t k = testutil_random(&rnd) % 64; /* 0..63 */
                if (k == 0)
                    vals[i] = 0;
                else if (testutil_random(&rnd) & 1)
                    vals[i] = (1ULL << k) - 1ULL; /* 2^k - 1 */
                else
                    vals[i] = (k == 63 ? (1ULL << 63) : (1ULL << k)); /* 2^k */
                break;
            }
            case 4:
                vals[i] = rand_u64(&rnd) & 0xFFFFULL; /* lower 16 bits */
                break;
            default:
                vals[i] = rand_u64(&rnd) >> (testutil_random(&rnd) % 64);
                break;
            }
        }

        encode_array(vals, n, buf, sizeof(buf), &used);
        assert_bytes_for_values(used, vals, n);
        decode_array(buf, used, n, out);
        for (size_t i = 0; i < n; ++i)
            testutil_assert(out[i] == vals[i]);
    }

    /* Zigzag signed integers fuzz */
    for (int iter = 0; iter < 2000; ++iter) {
        int64_t svals[64], sdec[64];
        uint64_t enc[64], outpos[64];
        size_t n = (size_t)(testutil_random(&rnd) % WT_ELEMENTS(svals));

        for (size_t i = 0; i < n; ++i) {
            if (testutil_random(&rnd) % 3 == 0) {
                int32_t x = (int32_t)testutil_random(&rnd);
                svals[i] = (int64_t)(x % 1001); /* small signed range [-1000,1000] */
            } else {
                svals[i] = (int64_t)rand_u64(&rnd); /* full 64-bit space */
            }
            enc[i] = __wt_encode_signed_as_positive(svals[i]);
        }

        encode_array(enc, n, buf, sizeof(buf), &used);
        assert_bytes_for_values(used, enc, n);
        decode_array(buf, used, n, outpos);
        for (size_t i = 0; i < n; ++i) {
            sdec[i] = __wt_decode_positive_as_signed(outpos[i]);
            testutil_assert(sdec[i] == svals[i]);
        }
    }
}

/*
 * main --
 *     Main.
 */
int
main(void)
{
    /*
     * Required on some systems to pull in parts of the library for which we have data references.
     */
    testutil_check(__wt_library_init());

    test_positive_integers();
    test_signed_integers();
    test_pairs_of_integers();
    test_small_int_arrays();
    test_bigger_int_arrays();

    /* Additional corner-case tests */
    test_extreme_values();
    test_alignment_boundaries();
    test_exact_fit_and_enomem();
    test_truncated_and_overcount_decode();
    test_partial_decode_resume();

    /* Randomized fuzz tests */
    test_random_fuzz();

    return (0);
}
