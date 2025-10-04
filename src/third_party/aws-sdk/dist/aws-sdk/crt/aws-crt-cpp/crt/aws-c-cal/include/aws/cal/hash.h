#ifndef AWS_CAL_HASH_H_
#define AWS_CAL_HASH_H_
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/exports.h>

#include <aws/common/byte_buf.h>
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_SHA256_LEN 32
#define AWS_SHA1_LEN 20
#define AWS_MD5_LEN 16

struct aws_hash;

struct aws_hash_vtable {
    const char *alg_name;
    const char *provider;
    void (*destroy)(struct aws_hash *hash);
    int (*update)(struct aws_hash *hash, const struct aws_byte_cursor *buf);
    int (*finalize)(struct aws_hash *hash, struct aws_byte_buf *out);
};

struct aws_hash {
    struct aws_allocator *allocator;
    struct aws_hash_vtable *vtable;
    size_t digest_size;
    bool good;
    void *impl;
};

typedef struct aws_hash *(aws_hash_new_fn)(struct aws_allocator *allocator);

AWS_EXTERN_C_BEGIN
/**
 * Allocates and initializes a sha256 hash instance.
 */
AWS_CAL_API struct aws_hash *aws_sha256_new(struct aws_allocator *allocator);
/**
 * Allocates and initializes a sha1 hash instance.
 */
AWS_CAL_API struct aws_hash *aws_sha1_new(struct aws_allocator *allocator);
/**
 * Allocates and initializes an md5 hash instance.
 */
AWS_CAL_API struct aws_hash *aws_md5_new(struct aws_allocator *allocator);

/**
 * Cleans up and deallocates hash.
 */
AWS_CAL_API void aws_hash_destroy(struct aws_hash *hash);
/**
 * Updates the running hash with to_hash. this can be called multiple times.
 */
AWS_CAL_API int aws_hash_update(struct aws_hash *hash, const struct aws_byte_cursor *to_hash);
/**
 * Completes the hash computation and writes the final digest to output.
 * Allocation of output is the caller's responsibility. If you specify
 * truncate_to to something other than 0, the output will be truncated to that
 * number of bytes. For example, if you want a SHA256 digest as the first 16
 * bytes, set truncate_to to 16. If you want the full digest size, just set this
 * to 0.
 */
AWS_CAL_API int aws_hash_finalize(struct aws_hash *hash, struct aws_byte_buf *output, size_t truncate_to);

/**
 * Computes the md5 hash over input and writes the digest output to 'output'.
 * Use this if you don't need to stream the data you're hashing and you can load
 * the entire input to hash into memory.
 */
AWS_CAL_API int aws_md5_compute(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *input,
    struct aws_byte_buf *output,
    size_t truncate_to);

/**
 * Computes the sha256 hash over input and writes the digest output to 'output'.
 * Use this if you don't need to stream the data you're hashing and you can load
 * the entire input to hash into memory. If you specify truncate_to to something
 * other than 0, the output will be truncated to that number of bytes. For
 * example, if you want a SHA256 digest as the first 16 bytes, set truncate_to
 * to 16. If you want the full digest size, just set this to 0.
 */
AWS_CAL_API int aws_sha256_compute(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *input,
    struct aws_byte_buf *output,
    size_t truncate_to);

/**
 * Computes the sha1 hash over input and writes the digest output to 'output'.
 * Use this if you don't need to stream the data you're hashing and you can load
 * the entire input to hash into memory. If you specify truncate_to to something
 * other than 0, the output will be truncated to that number of bytes. For
 * example, if you want a SHA1 digest as the first 16 bytes, set truncate_to
 * to 16. If you want the full digest size, just set this to 0.
 */
AWS_CAL_API int aws_sha1_compute(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *input,
    struct aws_byte_buf *output,
    size_t truncate_to);

/**
 * Set the implementation of md5 to use. If you compiled without BYO_CRYPTO,
 * you do not need to call this. However, if use this, we will honor it,
 * regardless of compile options. This may be useful for testing purposes. If
 * you did set BYO_CRYPTO, and you do not call this function you will
 * segfault.
 */
AWS_CAL_API void aws_set_md5_new_fn(aws_hash_new_fn *fn);

/**
 * Set the implementation of sha256 to use. If you compiled without
 * BYO_CRYPTO, you do not need to call this. However, if use this, we will
 * honor it, regardless of compile options. This may be useful for testing
 * purposes. If you did set BYO_CRYPTO, and you do not call this function
 * you will segfault.
 */
AWS_CAL_API void aws_set_sha256_new_fn(aws_hash_new_fn *fn);

/**
 * Set the implementation of sha1 to use. If you compiled without
 * BYO_CRYPTO, you do not need to call this. However, if use this, we will
 * honor it, regardless of compile options. This may be useful for testing
 * purposes. If you did set BYO_CRYPTO, and you do not call this function
 * you will segfault.
 */
AWS_CAL_API void aws_set_sha1_new_fn(aws_hash_new_fn *fn);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_CAL_HASH_H_ */
