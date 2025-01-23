#ifndef AWS_CAL_HMAC_H_
#define AWS_CAL_HMAC_H_
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/exports.h>

#include <aws/common/byte_buf.h>
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_SHA256_HMAC_LEN 32

struct aws_hmac;

struct aws_hmac_vtable {
    const char *alg_name;
    const char *provider;
    void (*destroy)(struct aws_hmac *hmac);
    int (*update)(struct aws_hmac *hmac, const struct aws_byte_cursor *buf);
    int (*finalize)(struct aws_hmac *hmac, struct aws_byte_buf *out);
};

struct aws_hmac {
    struct aws_allocator *allocator;
    struct aws_hmac_vtable *vtable;
    size_t digest_size;
    bool good;
    void *impl;
};

typedef struct aws_hmac *(aws_hmac_new_fn)(struct aws_allocator *allocator, const struct aws_byte_cursor *secret);

AWS_EXTERN_C_BEGIN
/**
 * Allocates and initializes a sha256 hmac instance. Secret is the key to be
 * used for the hmac process.
 */
AWS_CAL_API struct aws_hmac *aws_sha256_hmac_new(struct aws_allocator *allocator, const struct aws_byte_cursor *secret);

/**
 * Cleans up and deallocates hmac.
 */
AWS_CAL_API void aws_hmac_destroy(struct aws_hmac *hmac);

/**
 * Updates the running hmac with to_hash. this can be called multiple times.
 */
AWS_CAL_API int aws_hmac_update(struct aws_hmac *hmac, const struct aws_byte_cursor *to_hmac);
/**
 * Completes the hmac computation and writes the final digest to output.
 * Allocation of output is the caller's responsibility. If you specify
 * truncate_to to something other than 0, the output will be truncated to that
 * number of bytes. For example if you want a SHA256 digest as the first 16
 * bytes, set truncate_to to 16. If you want the full digest size, just set this
 * to 0.
 */
AWS_CAL_API int aws_hmac_finalize(struct aws_hmac *hmac, struct aws_byte_buf *output, size_t truncate_to);
/**
 * Computes the sha256 hmac over input and writes the digest output to 'output'.
 * Use this if you don't need to stream the data you're hashing and you can load
 * the entire input to hash into memory. If you specify truncate_to to something
 * other than 0, the output will be truncated to that number of bytes. For
 * example if you want a SHA256 HMAC digest as the first 16 bytes, set
 * truncate_to to 16. If you want the full digest size, just set this to 0.
 */
AWS_CAL_API int aws_sha256_hmac_compute(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *secret,
    const struct aws_byte_cursor *to_hmac,
    struct aws_byte_buf *output,
    size_t truncate_to);
/**
 * Set the implementation of sha256 hmac to use. If you compiled without
 * BYO_CRYPTO, you do not need to call this. However, if use this, we will
 * honor it, regardless of compile options. This may be useful for testing
 * purposes. If you did set BYO_CRYPTO, and you do not call this function
 * you will segfault.
 */
AWS_CAL_API void aws_set_sha256_hmac_new_fn(aws_hmac_new_fn *fn);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_CAL_HASH_H_ */
