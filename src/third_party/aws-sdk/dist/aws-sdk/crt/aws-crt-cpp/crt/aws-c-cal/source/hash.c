/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/hash.h>

#ifndef BYO_CRYPTO
extern struct aws_hash *aws_sha256_default_new(struct aws_allocator *allocator);
extern struct aws_hash *aws_sha1_default_new(struct aws_allocator *allocator);
extern struct aws_hash *aws_md5_default_new(struct aws_allocator *allocator);

static aws_hash_new_fn *s_sha256_new_fn = aws_sha256_default_new;
static aws_hash_new_fn *s_sha1_new_fn = aws_sha1_default_new;
static aws_hash_new_fn *s_md5_new_fn = aws_md5_default_new;
#else
static struct aws_hash *aws_hash_new_abort(struct aws_allocator *allocator) {
    (void)allocator;
    abort();
}

static aws_hash_new_fn *s_sha256_new_fn = aws_hash_new_abort;
static aws_hash_new_fn *s_sha1_new_fn = aws_hash_new_abort;
static aws_hash_new_fn *s_md5_new_fn = aws_hash_new_abort;
#endif

struct aws_hash *aws_sha1_new(struct aws_allocator *allocator) {
    return s_sha1_new_fn(allocator);
}

struct aws_hash *aws_sha256_new(struct aws_allocator *allocator) {
    return s_sha256_new_fn(allocator);
}

struct aws_hash *aws_md5_new(struct aws_allocator *allocator) {
    return s_md5_new_fn(allocator);
}

void aws_set_md5_new_fn(aws_hash_new_fn *fn) {
    s_md5_new_fn = fn;
}

void aws_set_sha256_new_fn(aws_hash_new_fn *fn) {
    s_sha256_new_fn = fn;
}

void aws_set_sha1_new_fn(aws_hash_new_fn *fn) {
    s_sha1_new_fn = fn;
}

void aws_hash_destroy(struct aws_hash *hash) {
    hash->vtable->destroy(hash);
}

int aws_hash_update(struct aws_hash *hash, const struct aws_byte_cursor *to_hash) {
    return hash->vtable->update(hash, to_hash);
}

int aws_hash_finalize(struct aws_hash *hash, struct aws_byte_buf *output, size_t truncate_to) {

    if (truncate_to && truncate_to < hash->digest_size) {
        size_t available_buffer = output->capacity - output->len;
        if (available_buffer < truncate_to) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }

        uint8_t tmp_output[128] = {0};
        AWS_ASSERT(sizeof(tmp_output) >= hash->digest_size);

        struct aws_byte_buf tmp_out_buf = aws_byte_buf_from_array(tmp_output, sizeof(tmp_output));
        tmp_out_buf.len = 0;

        if (hash->vtable->finalize(hash, &tmp_out_buf)) {
            return AWS_OP_ERR;
        }

        memcpy(output->buffer + output->len, tmp_output, truncate_to);
        output->len += truncate_to;
        return AWS_OP_SUCCESS;
    }

    return hash->vtable->finalize(hash, output);
}

static inline int compute_hash(
    struct aws_hash *hash,
    const struct aws_byte_cursor *input,
    struct aws_byte_buf *output,
    size_t truncate_to) {
    if (!hash) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (aws_hash_update(hash, input)) {
        aws_hash_destroy(hash);
        return AWS_OP_ERR;
    }

    if (aws_hash_finalize(hash, output, truncate_to)) {
        aws_hash_destroy(hash);
        return AWS_OP_ERR;
    }

    aws_hash_destroy(hash);
    return AWS_OP_SUCCESS;
}

int aws_md5_compute(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *input,
    struct aws_byte_buf *output,
    size_t truncate_to) {
    return compute_hash(aws_md5_new(allocator), input, output, truncate_to);
}

int aws_sha256_compute(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *input,
    struct aws_byte_buf *output,
    size_t truncate_to) {
    return compute_hash(aws_sha256_new(allocator), input, output, truncate_to);
}

int aws_sha1_compute(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *input,
    struct aws_byte_buf *output,
    size_t truncate_to) {
    return compute_hash(aws_sha1_new(allocator), input, output, truncate_to);
}
