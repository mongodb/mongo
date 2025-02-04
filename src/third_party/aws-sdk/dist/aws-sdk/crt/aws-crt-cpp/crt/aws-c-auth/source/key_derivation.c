/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/private/key_derivation.h>

#include <aws/auth/credentials.h>
#include <aws/cal/ecc.h>
#include <aws/cal/hash.h>
#include <aws/cal/hmac.h>
#include <aws/common/byte_buf.h>
#include <aws/common/string.h>

/*
 * The maximum number of iterations we will attempt to derive a valid ecc key for.  The probability that this counter
 * value ever gets reached is vanishingly low -- with reasonable uniformity/independence assumptions, it's
 * approximately
 *
 *  2 ^ (-32 * 254)
 */
#define MAX_KEY_DERIVATION_COUNTER_VALUE 254

/*
 * The encoding (32-bit, big-endian) of the prefix to the FixedInputString when fed to the hmac function, per
 * the sigv4a key derivation specification.
 */
AWS_STATIC_STRING_FROM_LITERAL(s_1_as_four_bytes_be, "\x00\x00\x00\x01");

/*
 * The encoding (32-bit, big-endian) of the "Length" component of the sigv4a key derivation specification
 */
AWS_STATIC_STRING_FROM_LITERAL(s_256_as_four_bytes_be, "\x00\x00\x01\x00");

AWS_STRING_FROM_LITERAL(g_signature_type_sigv4a_http_request, "AWS4-ECDSA-P256-SHA256");

AWS_STATIC_STRING_FROM_LITERAL(s_secret_buffer_prefix, "AWS4A");

/*
 * This constructs the fixed input byte sequence of the Sigv4a key derivation specification.  It also includes the
 * value (0x01 as a 32-bit big endian value) that is pre-pended to the fixed input before invoking the hmac to
 * generate the candidate key value.
 *
 * The final output looks like
 *
 * 0x00000001 || "AWS4-ECDSA-P256-SHA256" || 0x00 || AccessKeyId || CounterValue as uint8_t || 0x00000100 (Length)
 *
 * From this, we can determine the necessary buffer capacity when setting up the fixed input buffer:
 *
 * 4 + 22 + 1 + len(AccessKeyId) + 1 + 4 = 32 + len(AccessKeyId)
 */
static int s_aws_build_fixed_input_buffer(
    struct aws_byte_buf *fixed_input,
    const struct aws_credentials *credentials,
    const uint8_t counter) {

    if (counter == 0 || counter > MAX_KEY_DERIVATION_COUNTER_VALUE) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    if (!aws_byte_buf_is_valid(fixed_input)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    aws_byte_buf_reset(fixed_input, false);

    /*
     * A placeholder value that's not actually part of the fixed input string in the spec, but is always this value
     * and is always the first byte of the hmac-ed string.
     */
    struct aws_byte_cursor one_cursor = aws_byte_cursor_from_string(s_1_as_four_bytes_be);
    if (aws_byte_buf_append_dynamic(fixed_input, &one_cursor)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor sigv4a_algorithm_cursor = aws_byte_cursor_from_string(g_signature_type_sigv4a_http_request);
    if (aws_byte_buf_append(fixed_input, &sigv4a_algorithm_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(fixed_input, 0)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor access_key_cursor = aws_credentials_get_access_key_id(credentials);
    if (aws_byte_buf_append(fixed_input, &access_key_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(fixed_input, counter)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor encoded_bit_length_cursor = aws_byte_cursor_from_string(s_256_as_four_bytes_be);
    if (aws_byte_buf_append_dynamic(fixed_input, &encoded_bit_length_cursor)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*
 * aws_be_bytes_compare_constant_time() and aws_be_bytes_add_one_constant_time() are constant-time arithmetic functions
 * that operate on raw bytes as if they were unbounded integers in a big-endian base 255 format.
 */

/*
 * In the following function gt and eq are updated together.  After each update, the variables will be
 * in one of the following states:
 *
 *  (1) gt is 0, eq is 1, and from an ordering perspective, lhs == rhs, as checked "so far"
 *  (2) gt is 1, eq is 0, (lhs > rhs)
 *  (3) gt is 0, eq is 0, (lhs < rhs)
 *
 *  States (2) and (3) are terminal states that cannot be exited since eq is 0 and is the and-wise mask of all
 *  subsequent gt updates.  Similarly, once eq is zero it cannot ever become non-zero.
 *
 *  Intuitively these ideas match the standard way of comparing magnitude equality by considering digit count and
 *  digits from most significant to least significant.
 *
 *  Let l and r be the the two digits that we are
 *  comparing between lhs and rhs.  Assume 0 <= l, r <= 255 seated in 32-bit integers
 *
 *  gt is maintained by the following bit trick:
 *
 *      l > r <=>
 *      (r - l) < 0 <=>
 *      (r - l) as an int32 has the high bit set <=>
 *      ((r - l) >> 31) & 0x01 == 1
 *
 *  eq is maintained by the following bit trick:
 *
 *      l == r <=>
 *      l ^ r == 0 <=>
 *      (l ^ r) - 1 == -1 <=>
 *      (((l ^ r) - 1) >> 31) & 0x01 == 1
 *
 *  We apply to the volatile type modifier to attempt to prevent all early-out optimizations that a compiler might
 *  apply if it performed constraint-based reasoning on the logic.  This is based on treating volatile
 *  semantically as "this value can change underneath you at any time so you always have to re-read it and cannot
 *  reason statically about program behavior when it reaches a certain value (like 0)"
 */

/**
 * Compares two large unsigned integers in a raw byte format.
 * The two operands *must* be the same size (simplifies the problem significantly).
 *
 * The output parameter comparison_result is set to:
 *   -1 if lhs_raw_be_bigint < rhs_raw_be_bigint
 *    0 if lhs_raw_be_bigint == rhs_raw_be_bigint
 *    1 if lhs_raw_be_bigint > rhs_raw_be_bigint
 */
int aws_be_bytes_compare_constant_time(
    const struct aws_byte_buf *lhs_raw_be_bigint,
    const struct aws_byte_buf *rhs_raw_be_bigint,
    int *comparison_result) {

    AWS_FATAL_PRECONDITION(aws_byte_buf_is_valid(lhs_raw_be_bigint));
    AWS_FATAL_PRECONDITION(aws_byte_buf_is_valid(rhs_raw_be_bigint));

    /*
     * We only need to support comparing byte sequences of the same length here
     */
    const size_t lhs_len = lhs_raw_be_bigint->len;
    if (lhs_len != rhs_raw_be_bigint->len) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    volatile uint8_t gt = 0;
    volatile uint8_t eq = 1;

    const uint8_t *lhs_raw_bytes = lhs_raw_be_bigint->buffer;
    const uint8_t *rhs_raw_bytes = rhs_raw_be_bigint->buffer;
    for (size_t i = 0; i < lhs_len; ++i) {
        volatile int32_t lhs_digit = (int32_t)lhs_raw_bytes[i];
        volatile int32_t rhs_digit = (int32_t)rhs_raw_bytes[i];

        /*
         * For each digit, check for a state (1) => (2) ie lhs > rhs, or (1) => (3) ie lhs < rhs transition
         * based on comparing the two digits in constant time using the ideas explained in the giant comment
         * block above this function.
         */
        gt |= ((rhs_digit - lhs_digit) >> 31) & eq;
        eq &= (((lhs_digit ^ rhs_digit) - 1) >> 31) & 0x01;
    }

    *comparison_result = gt + gt + eq - 1;

    return AWS_OP_SUCCESS;
}

/**
 * Adds one to a large unsigned integer represented by a sequence of bytes.
 *
 * A maximal value will roll over to zero.  This does not affect the correctness of the users
 * of this function.
 */
void aws_be_bytes_add_one_constant_time(struct aws_byte_buf *raw_be_bigint) {
    AWS_FATAL_PRECONDITION(aws_byte_buf_is_valid(raw_be_bigint));

    const size_t byte_count = raw_be_bigint->len;

    volatile uint32_t carry = 1;
    uint8_t *raw_bytes = raw_be_bigint->buffer;

    for (size_t i = 0; i < byte_count; ++i) {
        const size_t index = byte_count - i - 1;

        volatile uint32_t current_digit = raw_bytes[index];
        current_digit += carry;

        carry = (current_digit >> 8) & 0x01;

        raw_bytes[index] = (uint8_t)(current_digit & 0xFF);
    }
}

/* clang-format off */

/* In the spec, this is N-2 */
static uint8_t s_n_minus_2[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
    0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x4F,
};

/* clang-format on */

enum aws_key_derivation_result {
    AKDR_SUCCESS,
    AKDR_NEXT_COUNTER,
    AKDR_FAILURE,
};

static enum aws_key_derivation_result s_aws_derive_ecc_private_key(
    struct aws_byte_buf *private_key_value,
    const struct aws_byte_buf *k0) {
    AWS_FATAL_ASSERT(k0->len == aws_ecc_key_coordinate_byte_size_from_curve_name(AWS_CAL_ECDSA_P256));

    aws_byte_buf_reset(private_key_value, false);

    struct aws_byte_buf s_n_minus_2_buf = {
        .allocator = NULL,
        .buffer = s_n_minus_2,
        .capacity = AWS_ARRAY_SIZE(s_n_minus_2),
        .len = AWS_ARRAY_SIZE(s_n_minus_2),
    };

    int comparison_result = 0;
    if (aws_be_bytes_compare_constant_time(k0, &s_n_minus_2_buf, &comparison_result)) {
        return AKDR_FAILURE;
    }

    if (comparison_result > 0) {
        return AKDR_NEXT_COUNTER;
    }

    struct aws_byte_cursor k0_cursor = aws_byte_cursor_from_buf(k0);
    if (aws_byte_buf_append(private_key_value, &k0_cursor)) {
        return AKDR_FAILURE;
    }

    aws_be_bytes_add_one_constant_time(private_key_value);

    return AKDR_SUCCESS;
}

static int s_init_secret_buf(
    struct aws_byte_buf *secret_buf,
    struct aws_allocator *allocator,
    const struct aws_credentials *credentials) {

    struct aws_byte_cursor secret_access_key_cursor = aws_credentials_get_secret_access_key(credentials);
    size_t secret_buffer_length = secret_access_key_cursor.len + s_secret_buffer_prefix->len;
    if (aws_byte_buf_init(secret_buf, allocator, secret_buffer_length)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor prefix_cursor = aws_byte_cursor_from_string(s_secret_buffer_prefix);
    if (aws_byte_buf_append(secret_buf, &prefix_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append(secret_buf, &secret_access_key_cursor)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

struct aws_ecc_key_pair *aws_ecc_key_pair_new_ecdsa_p256_key_from_aws_credentials(
    struct aws_allocator *allocator,
    const struct aws_credentials *credentials) {

    if (allocator == NULL || credentials == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_ecc_key_pair *ecc_key_pair = NULL;

    struct aws_byte_buf fixed_input;
    AWS_ZERO_STRUCT(fixed_input);

    struct aws_byte_buf fixed_input_hmac_digest;
    AWS_ZERO_STRUCT(fixed_input_hmac_digest);

    struct aws_byte_buf private_key_buf;
    AWS_ZERO_STRUCT(private_key_buf);

    struct aws_byte_buf secret_buf;
    AWS_ZERO_STRUCT(secret_buf);

    size_t access_key_length = aws_credentials_get_access_key_id(credentials).len;

    /*
     * This value is calculated based on the format of the fixed input string as described above at
     * the definition of s_aws_build_fixed_input_buffer()
     */
    size_t required_fixed_input_capacity = 32 + access_key_length;
    if (aws_byte_buf_init(&fixed_input, allocator, required_fixed_input_capacity)) {
        goto done;
    }

    if (aws_byte_buf_init(&fixed_input_hmac_digest, allocator, AWS_SHA256_LEN)) {
        goto done;
    }

    size_t key_length = aws_ecc_key_coordinate_byte_size_from_curve_name(AWS_CAL_ECDSA_P256);
    AWS_FATAL_ASSERT(key_length == AWS_SHA256_LEN);
    if (aws_byte_buf_init(&private_key_buf, allocator, key_length)) {
        goto done;
    }

    if (s_init_secret_buf(&secret_buf, allocator, credentials)) {
        goto done;
    }
    struct aws_byte_cursor secret_cursor = aws_byte_cursor_from_buf(&secret_buf);

    uint8_t counter = 1;
    enum aws_key_derivation_result result = AKDR_NEXT_COUNTER;
    while ((result == AKDR_NEXT_COUNTER) && (counter <= MAX_KEY_DERIVATION_COUNTER_VALUE)) {
        if (s_aws_build_fixed_input_buffer(&fixed_input, credentials, counter++)) {
            break;
        }

        aws_byte_buf_reset(&fixed_input_hmac_digest, true);

        struct aws_byte_cursor fixed_input_cursor = aws_byte_cursor_from_buf(&fixed_input);
        if (aws_sha256_hmac_compute(allocator, &secret_cursor, &fixed_input_cursor, &fixed_input_hmac_digest, 0)) {
            break;
        }

        result = s_aws_derive_ecc_private_key(&private_key_buf, &fixed_input_hmac_digest);
    }

    if (result == AKDR_SUCCESS) {
        struct aws_byte_cursor private_key_cursor = aws_byte_cursor_from_buf(&private_key_buf);
        ecc_key_pair = aws_ecc_key_pair_new_from_private_key(allocator, AWS_CAL_ECDSA_P256, &private_key_cursor);
    }

done:

    aws_byte_buf_clean_up_secure(&secret_buf);
    aws_byte_buf_clean_up_secure(&private_key_buf);
    aws_byte_buf_clean_up_secure(&fixed_input_hmac_digest);
    aws_byte_buf_clean_up(&fixed_input);

    return ecc_key_pair;
}
