/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/hmac.h>

#ifndef BYO_CRYPTO
extern struct aws_hmac *aws_sha256_hmac_default_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *secret);
static aws_hmac_new_fn *s_sha256_hmac_new_fn = aws_sha256_hmac_default_new;
#else
static struct aws_hmac *aws_hmac_new_abort(struct aws_allocator *allocator, const struct aws_byte_cursor *secret) {
    (void)allocator;
    (void)secret;
    abort();
}

static aws_hmac_new_fn *s_sha256_hmac_new_fn = aws_hmac_new_abort;
#endif

struct aws_hmac *aws_sha256_hmac_new(struct aws_allocator *allocator, const struct aws_byte_cursor *secret) {
    return s_sha256_hmac_new_fn(allocator, secret);
}

void aws_set_sha256_hmac_new_fn(aws_hmac_new_fn *fn) {
    s_sha256_hmac_new_fn = fn;
}

void aws_hmac_destroy(struct aws_hmac *hmac) {
    hmac->vtable->destroy(hmac);
}

int aws_hmac_update(struct aws_hmac *hmac, const struct aws_byte_cursor *to_hmac) {
    return hmac->vtable->update(hmac, to_hmac);
}

int aws_hmac_finalize(struct aws_hmac *hmac, struct aws_byte_buf *output, size_t truncate_to) {
    if (truncate_to && truncate_to < hmac->digest_size) {
        size_t available_buffer = output->capacity - output->len;
        if (available_buffer < truncate_to) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }

        uint8_t tmp_output[128] = {0};
        AWS_ASSERT(sizeof(tmp_output) >= hmac->digest_size);

        struct aws_byte_buf tmp_out_buf = aws_byte_buf_from_array(tmp_output, sizeof(tmp_output));
        tmp_out_buf.len = 0;

        if (hmac->vtable->finalize(hmac, &tmp_out_buf)) {
            return AWS_OP_ERR;
        }

        memcpy(output->buffer + output->len, tmp_output, truncate_to);
        output->len += truncate_to;
        return AWS_OP_SUCCESS;
    }

    return hmac->vtable->finalize(hmac, output);
}

int aws_sha256_hmac_compute(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *secret,
    const struct aws_byte_cursor *to_hmac,
    struct aws_byte_buf *output,
    size_t truncate_to) {
    struct aws_hmac *hmac = aws_sha256_hmac_new(allocator, secret);

    if (!hmac) {
        return AWS_OP_ERR;
    }

    if (aws_hmac_update(hmac, to_hmac)) {
        aws_hmac_destroy(hmac);
        return AWS_OP_ERR;
    }

    if (aws_hmac_finalize(hmac, output, truncate_to)) {
        aws_hmac_destroy(hmac);
        return AWS_OP_ERR;
    }

    aws_hmac_destroy(hmac);
    return AWS_OP_SUCCESS;
}
