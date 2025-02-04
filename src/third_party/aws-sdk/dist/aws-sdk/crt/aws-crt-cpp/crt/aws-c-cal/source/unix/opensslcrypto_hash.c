/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/hash.h>
#include <aws/cal/private/opensslcrypto_common.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

static void s_destroy(struct aws_hash *hash);
static int s_update(struct aws_hash *hash, const struct aws_byte_cursor *to_hash);
static int s_finalize(struct aws_hash *hash, struct aws_byte_buf *output);

static struct aws_hash_vtable s_md5_vtable = {
    .destroy = s_destroy,
    .update = s_update,
    .finalize = s_finalize,
    .alg_name = "MD5",
    .provider = "OpenSSL Compatible libcrypto",
};

static struct aws_hash_vtable s_sha256_vtable = {
    .destroy = s_destroy,
    .update = s_update,
    .finalize = s_finalize,
    .alg_name = "SHA256",
    .provider = "OpenSSL Compatible libcrypto",
};

static struct aws_hash_vtable s_sha1_vtable = {
    .destroy = s_destroy,
    .update = s_update,
    .finalize = s_finalize,
    .alg_name = "SHA1",
    .provider = "OpenSSL Compatible libcrypto",
};

static void s_destroy(struct aws_hash *hash) {
    if (hash == NULL) {
        return;
    }

    EVP_MD_CTX *ctx = hash->impl;
    if (ctx != NULL) {
        g_aws_openssl_evp_md_ctx_table->free_fn(ctx);
    }

    aws_mem_release(hash->allocator, hash);
}

struct aws_hash *aws_md5_default_new(struct aws_allocator *allocator) {
    struct aws_hash *hash = aws_mem_acquire(allocator, sizeof(struct aws_hash));

    if (!hash) {
        return NULL;
    }

    hash->allocator = allocator;
    hash->vtable = &s_md5_vtable;
    hash->digest_size = AWS_MD5_LEN;
    EVP_MD_CTX *ctx = g_aws_openssl_evp_md_ctx_table->new_fn();
    hash->impl = ctx;
    hash->good = true;

    if (!hash->impl) {
        s_destroy(hash);
        aws_raise_error(AWS_ERROR_OOM);
        return NULL;
    }

    if (!g_aws_openssl_evp_md_ctx_table->init_ex_fn(ctx, EVP_md5(), NULL)) {
        s_destroy(hash);
        aws_raise_error(AWS_ERROR_UNKNOWN);
        return NULL;
    }

    return hash;
}

struct aws_hash *aws_sha256_default_new(struct aws_allocator *allocator) {
    struct aws_hash *hash = aws_mem_acquire(allocator, sizeof(struct aws_hash));

    if (!hash) {
        return NULL;
    }

    hash->allocator = allocator;
    hash->vtable = &s_sha256_vtable;
    hash->digest_size = AWS_SHA256_LEN;
    EVP_MD_CTX *ctx = g_aws_openssl_evp_md_ctx_table->new_fn();
    hash->impl = ctx;
    hash->good = true;

    if (!hash->impl) {
        s_destroy(hash);
        aws_raise_error(AWS_ERROR_OOM);
        return NULL;
    }

    if (!g_aws_openssl_evp_md_ctx_table->init_ex_fn(ctx, EVP_sha256(), NULL)) {
        s_destroy(hash);
        aws_raise_error(AWS_ERROR_UNKNOWN);
        return NULL;
    }

    return hash;
}

struct aws_hash *aws_sha1_default_new(struct aws_allocator *allocator) {
    struct aws_hash *hash = aws_mem_acquire(allocator, sizeof(struct aws_hash));

    if (!hash) {
        return NULL;
    }

    hash->allocator = allocator;
    hash->vtable = &s_sha1_vtable;
    hash->digest_size = AWS_SHA1_LEN;
    EVP_MD_CTX *ctx = g_aws_openssl_evp_md_ctx_table->new_fn();
    hash->impl = ctx;
    hash->good = true;

    if (!hash->impl) {
        s_destroy(hash);
        aws_raise_error(AWS_ERROR_OOM);
        return NULL;
    }

    if (!g_aws_openssl_evp_md_ctx_table->init_ex_fn(ctx, EVP_sha1(), NULL)) {
        s_destroy(hash);
        aws_raise_error(AWS_ERROR_UNKNOWN);
        return NULL;
    }

    return hash;
}

static int s_update(struct aws_hash *hash, const struct aws_byte_cursor *to_hash) {
    if (!hash->good) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    EVP_MD_CTX *ctx = hash->impl;

    if (AWS_LIKELY(g_aws_openssl_evp_md_ctx_table->update_fn(ctx, to_hash->ptr, to_hash->len))) {
        return AWS_OP_SUCCESS;
    }

    hash->good = false;
    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}

static int s_finalize(struct aws_hash *hash, struct aws_byte_buf *output) {
    if (!hash->good) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    EVP_MD_CTX *ctx = hash->impl;

    size_t buffer_len = output->capacity - output->len;

    if (buffer_len < hash->digest_size) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    if (AWS_LIKELY(g_aws_openssl_evp_md_ctx_table->final_ex_fn(
            ctx, output->buffer + output->len, (unsigned int *)&buffer_len))) {
        output->len += hash->digest_size;
        hash->good = false;
        return AWS_OP_SUCCESS;
    }

    hash->good = false;
    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}
