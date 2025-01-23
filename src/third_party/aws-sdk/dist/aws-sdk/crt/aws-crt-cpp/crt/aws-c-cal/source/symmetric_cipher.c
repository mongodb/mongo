/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/private/symmetric_cipher_priv.h>
#include <aws/cal/symmetric_cipher.h>
#include <aws/common/device_random.h>

#ifndef BYO_CRYPTO

extern struct aws_symmetric_cipher *aws_aes_cbc_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv);

extern struct aws_symmetric_cipher *aws_aes_ctr_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv);

extern struct aws_symmetric_cipher *aws_aes_gcm_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv,
    const struct aws_byte_cursor *aad);

extern struct aws_symmetric_cipher *aws_aes_keywrap_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key);

#else /* BYO_CRYPTO */
struct aws_symmetric_cipher *aws_aes_cbc_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv) {
    (void)allocator;
    (void)key;
    (void)iv;
    abort();
}

struct aws_symmetric_cipher *aws_aes_ctr_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv) {
    (void)allocator;
    (void)key;
    (void)iv;
    abort();
}

struct aws_symmetric_cipher *aws_aes_gcm_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv,
    const struct aws_byte_cursor *aad) {
    (void)allocator;
    (void)key;
    (void)iv;
    (void)aad;
    abort();
}

struct aws_symmetric_cipher *aws_aes_keywrap_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key) {
    (void)allocator;
    (void)key;
    abort();
}

#endif /* BYO_CRYPTO */

static aws_aes_cbc_256_new_fn *s_aes_cbc_new_fn = aws_aes_cbc_256_new_impl;
static aws_aes_ctr_256_new_fn *s_aes_ctr_new_fn = aws_aes_ctr_256_new_impl;
static aws_aes_gcm_256_new_fn *s_aes_gcm_new_fn = aws_aes_gcm_256_new_impl;
static aws_aes_keywrap_256_new_fn *s_aes_keywrap_new_fn = aws_aes_keywrap_256_new_impl;

static int s_check_input_size_limits(const struct aws_symmetric_cipher *cipher, const struct aws_byte_cursor *input) {
    /* libcrypto uses int, not size_t, so this is the limit.
     * For simplicity, enforce the same rules on all platforms. */
    return input->len <= INT_MAX - cipher->block_size ? AWS_OP_SUCCESS
                                                      : aws_raise_error(AWS_ERROR_CAL_BUFFER_TOO_LARGE_FOR_ALGORITHM);
}

static int s_validate_key_materials(
    const struct aws_byte_cursor *key,
    size_t expected_key_size,
    const struct aws_byte_cursor *iv,
    size_t expected_iv_size) {
    if (key && key->len != expected_key_size) {
        return aws_raise_error(AWS_ERROR_CAL_INVALID_KEY_LENGTH_FOR_ALGORITHM);
    }

    if (iv && iv->len != expected_iv_size) {
        return aws_raise_error(AWS_ERROR_CAL_INVALID_CIPHER_MATERIAL_SIZE_FOR_ALGORITHM);
    }

    return AWS_OP_SUCCESS;
}

struct aws_symmetric_cipher *aws_aes_cbc_256_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv) {

    if (s_validate_key_materials(key, AWS_AES_256_KEY_BYTE_LEN, iv, AWS_AES_256_CIPHER_BLOCK_SIZE) != AWS_OP_SUCCESS) {
        return NULL;
    }
    return s_aes_cbc_new_fn(allocator, key, iv);
}

struct aws_symmetric_cipher *aws_aes_ctr_256_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv) {
    if (s_validate_key_materials(key, AWS_AES_256_KEY_BYTE_LEN, iv, AWS_AES_256_CIPHER_BLOCK_SIZE) != AWS_OP_SUCCESS) {
        return NULL;
    }
    return s_aes_ctr_new_fn(allocator, key, iv);
}

struct aws_symmetric_cipher *aws_aes_gcm_256_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv,
    const struct aws_byte_cursor *aad) {
    if (s_validate_key_materials(key, AWS_AES_256_KEY_BYTE_LEN, iv, AWS_AES_256_CIPHER_BLOCK_SIZE - sizeof(uint32_t)) !=
        AWS_OP_SUCCESS) {
        return NULL;
    }
    return s_aes_gcm_new_fn(allocator, key, iv, aad);
}

struct aws_symmetric_cipher *aws_aes_keywrap_256_new(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key) {
    if (s_validate_key_materials(key, AWS_AES_256_KEY_BYTE_LEN, NULL, 0) != AWS_OP_SUCCESS) {
        return NULL;
    }
    return s_aes_keywrap_new_fn(allocator, key);
}

void aws_symmetric_cipher_destroy(struct aws_symmetric_cipher *cipher) {
    if (cipher) {
        cipher->vtable->destroy(cipher);
    }
}

int aws_symmetric_cipher_encrypt(
    struct aws_symmetric_cipher *cipher,
    struct aws_byte_cursor to_encrypt,
    struct aws_byte_buf *out) {

    AWS_PRECONDITION(aws_byte_cursor_is_valid(&to_encrypt));

    if (AWS_UNLIKELY(s_check_input_size_limits(cipher, &to_encrypt) != AWS_OP_SUCCESS)) {
        return AWS_OP_ERR;
    }

    if (cipher->state == AWS_SYMMETRIC_CIPHER_READY) {
        return cipher->vtable->encrypt(cipher, to_encrypt, out);
    }

    return aws_raise_error(AWS_ERROR_INVALID_STATE);
}

int aws_symmetric_cipher_decrypt(
    struct aws_symmetric_cipher *cipher,
    struct aws_byte_cursor to_decrypt,
    struct aws_byte_buf *out) {

    AWS_PRECONDITION(aws_byte_cursor_is_valid(&to_decrypt));

    if (AWS_UNLIKELY(s_check_input_size_limits(cipher, &to_decrypt) != AWS_OP_SUCCESS)) {
        return AWS_OP_ERR;
    }

    if (cipher->state == AWS_SYMMETRIC_CIPHER_READY) {
        return cipher->vtable->decrypt(cipher, to_decrypt, out);
    }

    return aws_raise_error(AWS_ERROR_INVALID_STATE);
}

int aws_symmetric_cipher_finalize_encryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out) {
    if (cipher->state == AWS_SYMMETRIC_CIPHER_READY) {
        int ret_val = cipher->vtable->finalize_encryption(cipher, out);
        if (cipher->state != AWS_SYMMETRIC_CIPHER_ERROR) {
            cipher->state = AWS_SYMMETRIC_CIPHER_FINALIZED;
        }
        return ret_val;
    }

    return aws_raise_error(AWS_ERROR_INVALID_STATE);
}

int aws_symmetric_cipher_finalize_decryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out) {
    if (cipher->state == AWS_SYMMETRIC_CIPHER_READY) {
        int ret_val = cipher->vtable->finalize_decryption(cipher, out);
        if (cipher->state != AWS_SYMMETRIC_CIPHER_ERROR) {
            cipher->state = AWS_SYMMETRIC_CIPHER_FINALIZED;
        }
        return ret_val;
    }
    return aws_raise_error(AWS_ERROR_INVALID_STATE);
}

int aws_symmetric_cipher_reset(struct aws_symmetric_cipher *cipher) {
    int ret_val = cipher->vtable->reset(cipher);
    if (ret_val == AWS_OP_SUCCESS) {
        cipher->state = AWS_SYMMETRIC_CIPHER_READY;
    }

    return ret_val;
}

struct aws_byte_cursor aws_symmetric_cipher_get_tag(const struct aws_symmetric_cipher *cipher) {
    return aws_byte_cursor_from_buf(&cipher->tag);
}

void aws_symmetric_cipher_set_tag(struct aws_symmetric_cipher *cipher, struct aws_byte_cursor tag) {
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&tag));
    aws_byte_buf_clean_up_secure(&cipher->tag);
    aws_byte_buf_init_copy_from_cursor(&cipher->tag, cipher->allocator, tag);
}

struct aws_byte_cursor aws_symmetric_cipher_get_initialization_vector(const struct aws_symmetric_cipher *cipher) {
    return aws_byte_cursor_from_buf(&cipher->iv);
}

struct aws_byte_cursor aws_symmetric_cipher_get_key(const struct aws_symmetric_cipher *cipher) {
    return aws_byte_cursor_from_buf(&cipher->key);
}

bool aws_symmetric_cipher_is_good(const struct aws_symmetric_cipher *cipher) {
    return cipher->state == AWS_SYMMETRIC_CIPHER_READY;
}

enum aws_symmetric_cipher_state aws_symmetric_cipher_get_state(const struct aws_symmetric_cipher *cipher) {
    return cipher->state;
}

void aws_symmetric_cipher_generate_initialization_vector(
    size_t len_bytes,
    bool is_counter_mode,
    struct aws_byte_buf *out) {
    size_t counter_len = is_counter_mode ? sizeof(uint32_t) : 0;
    AWS_ASSERT(len_bytes > counter_len);
    size_t rand_len = len_bytes - counter_len;

    AWS_FATAL_ASSERT(aws_device_random_buffer_append(out, rand_len) == AWS_OP_SUCCESS);

    if (is_counter_mode) {
        /* put counter at the end, initialized to 1 */
        aws_byte_buf_write_be32(out, 1);
    }
}

void aws_symmetric_cipher_generate_key(size_t key_len_bytes, struct aws_byte_buf *out) {
    AWS_FATAL_ASSERT(aws_device_random_buffer_append(out, key_len_bytes) == AWS_OP_SUCCESS);
}

int aws_symmetric_cipher_try_ensure_sufficient_buffer_space(struct aws_byte_buf *buf, size_t size) {
    if (buf->capacity - buf->len < size) {
        return aws_byte_buf_reserve_relative(buf, size);
    }

    return AWS_OP_SUCCESS;
}
