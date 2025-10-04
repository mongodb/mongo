/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/private/symmetric_cipher_priv.h>

#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/evp.h>

struct openssl_aes_cipher {
    struct aws_symmetric_cipher cipher_base;
    EVP_CIPHER_CTX *encryptor_ctx;
    EVP_CIPHER_CTX *decryptor_ctx;
    struct aws_byte_buf working_buffer;
};

static struct aws_byte_cursor s_empty_plain_text = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("");

static int s_encrypt(struct aws_symmetric_cipher *cipher, struct aws_byte_cursor input, struct aws_byte_buf *out) {

    /*
     * Openssl 1.1.1 and its derivatives like aws-lc and boringssl do not handle
     * the case of null input of 0 len gracefully in update (it succeeds, but
     * finalize after it will fail). Openssl 3.0 fixed this. Other crypto implementations
     * do not have similar issue.
     * To workaround the issue, replace null cursor with empty cursor.
     */
    if (input.len == 0) {
        input = s_empty_plain_text;
    }

    size_t required_buffer_space = input.len + cipher->block_size;

    if (aws_symmetric_cipher_try_ensure_sufficient_buffer_space(out, required_buffer_space)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    size_t available_write_space = out->capacity - out->len;
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    int len_written = (int)(available_write_space);
    if (!EVP_EncryptUpdate(
            openssl_cipher->encryptor_ctx, out->buffer + out->len, &len_written, input.ptr, (int)input.len)) {
        cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    out->len += len_written;
    return AWS_OP_SUCCESS;
}

static int s_finalize_encryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    size_t required_buffer_space = cipher->block_size;

    if (aws_symmetric_cipher_try_ensure_sufficient_buffer_space(out, required_buffer_space)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    int len_written = (int)(out->capacity - out->len);
    if (!EVP_EncryptFinal_ex(openssl_cipher->encryptor_ctx, out->buffer + out->len, &len_written)) {
        cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    out->len += len_written;
    return AWS_OP_SUCCESS;
}

static int s_decrypt(struct aws_symmetric_cipher *cipher, struct aws_byte_cursor input, struct aws_byte_buf *out) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    size_t required_buffer_space = input.len + cipher->block_size;

    if (aws_symmetric_cipher_try_ensure_sufficient_buffer_space(out, required_buffer_space)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    size_t available_write_space = out->capacity - out->len;

    int len_written = (int)available_write_space;
    if (!EVP_DecryptUpdate(
            openssl_cipher->decryptor_ctx, out->buffer + out->len, &len_written, input.ptr, (int)input.len)) {
        cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    out->len += len_written;
    return AWS_OP_SUCCESS;
}

static int s_finalize_decryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    size_t required_buffer_space = cipher->block_size;

    if (aws_symmetric_cipher_try_ensure_sufficient_buffer_space(out, required_buffer_space)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    int len_written = (int)out->capacity - out->len;
    if (!EVP_DecryptFinal_ex(openssl_cipher->decryptor_ctx, out->buffer + out->len, &len_written)) {
        cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    out->len += len_written;
    return AWS_OP_SUCCESS;
}

static void s_destroy(struct aws_symmetric_cipher *cipher) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (openssl_cipher->encryptor_ctx) {
        EVP_CIPHER_CTX_free(openssl_cipher->encryptor_ctx);
    }

    if (openssl_cipher->decryptor_ctx) {
        EVP_CIPHER_CTX_free(openssl_cipher->decryptor_ctx);
    }

    aws_byte_buf_clean_up_secure(&cipher->key);
    aws_byte_buf_clean_up_secure(&cipher->iv);

    aws_byte_buf_clean_up_secure(&cipher->tag);

    if (cipher->aad.buffer) {
        aws_byte_buf_clean_up_secure(&cipher->aad);
    }

    aws_byte_buf_clean_up_secure(&openssl_cipher->working_buffer);

    aws_mem_release(cipher->allocator, openssl_cipher);
}

static int s_clear_reusable_state(struct aws_symmetric_cipher *cipher) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    EVP_CIPHER_CTX_cleanup(openssl_cipher->encryptor_ctx);
    EVP_CIPHER_CTX_cleanup(openssl_cipher->decryptor_ctx);
    aws_byte_buf_secure_zero(&openssl_cipher->working_buffer);
    cipher->state = AWS_SYMMETRIC_CIPHER_READY;
    return AWS_OP_SUCCESS;
}

static int s_init_cbc_cipher_materials(struct aws_symmetric_cipher *cipher) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (!EVP_EncryptInit_ex(
            openssl_cipher->encryptor_ctx,
            EVP_aes_256_cbc(),
            NULL,
            openssl_cipher->cipher_base.key.buffer,
            openssl_cipher->cipher_base.iv.buffer) ||
        !EVP_DecryptInit_ex(
            openssl_cipher->decryptor_ctx,
            EVP_aes_256_cbc(),
            NULL,
            openssl_cipher->cipher_base.key.buffer,
            openssl_cipher->cipher_base.iv.buffer)) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return AWS_OP_SUCCESS;
}

static int s_reset_cbc_cipher_materials(struct aws_symmetric_cipher *cipher) {
    int ret_val = s_clear_reusable_state(cipher);

    if (ret_val == AWS_OP_SUCCESS) {
        return s_init_cbc_cipher_materials(cipher);
    }

    return ret_val;
}

static struct aws_symmetric_cipher_vtable s_cbc_vtable = {
    .alg_name = "AES-CBC 256",
    .provider = "OpenSSL Compatible LibCrypto",
    .destroy = s_destroy,
    .reset = s_reset_cbc_cipher_materials,
    .decrypt = s_decrypt,
    .encrypt = s_encrypt,
    .finalize_decryption = s_finalize_decryption,
    .finalize_encryption = s_finalize_encryption,
};

struct aws_symmetric_cipher *aws_aes_cbc_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv) {
    struct openssl_aes_cipher *cipher = aws_mem_calloc(allocator, 1, sizeof(struct openssl_aes_cipher));

    cipher->cipher_base.allocator = allocator;
    cipher->cipher_base.block_size = AWS_AES_256_CIPHER_BLOCK_SIZE;
    cipher->cipher_base.key_length_bits = AWS_AES_256_KEY_BIT_LEN;
    cipher->cipher_base.vtable = &s_cbc_vtable;
    cipher->cipher_base.impl = cipher;

    if (key) {
        aws_byte_buf_init_copy_from_cursor(&cipher->cipher_base.key, allocator, *key);
    } else {
        aws_byte_buf_init(&cipher->cipher_base.key, allocator, AWS_AES_256_KEY_BYTE_LEN);
        aws_symmetric_cipher_generate_key(AWS_AES_256_KEY_BYTE_LEN, &cipher->cipher_base.key);
    }

    if (iv) {
        aws_byte_buf_init_copy_from_cursor(&cipher->cipher_base.iv, allocator, *iv);
    } else {
        aws_byte_buf_init(&cipher->cipher_base.iv, allocator, AWS_AES_256_CIPHER_BLOCK_SIZE);
        aws_symmetric_cipher_generate_initialization_vector(
            AWS_AES_256_CIPHER_BLOCK_SIZE, false, &cipher->cipher_base.iv);
    }

    /* EVP_CIPHER_CTX_init() will be called inside EVP_CIPHER_CTX_new(). */
    cipher->encryptor_ctx = EVP_CIPHER_CTX_new();
    AWS_FATAL_ASSERT(cipher->encryptor_ctx && "Cipher initialization failed!");

    /* EVP_CIPHER_CTX_init() will be called inside EVP_CIPHER_CTX_new(). */
    cipher->decryptor_ctx = EVP_CIPHER_CTX_new();
    AWS_FATAL_ASSERT(cipher->decryptor_ctx && "Cipher initialization failed!");

    if (s_init_cbc_cipher_materials(&cipher->cipher_base) != AWS_OP_SUCCESS) {
        goto error;
    }

    cipher->cipher_base.state = AWS_SYMMETRIC_CIPHER_READY;
    return &cipher->cipher_base;

error:
    s_destroy(&cipher->cipher_base);
    return NULL;
}

static int s_init_ctr_cipher_materials(struct aws_symmetric_cipher *cipher) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (!(EVP_EncryptInit_ex(
              openssl_cipher->encryptor_ctx,
              EVP_aes_256_ctr(),
              NULL,
              openssl_cipher->cipher_base.key.buffer,
              openssl_cipher->cipher_base.iv.buffer) &&
          EVP_CIPHER_CTX_set_padding(openssl_cipher->encryptor_ctx, 0)) ||
        !(EVP_DecryptInit_ex(
              openssl_cipher->decryptor_ctx,
              EVP_aes_256_ctr(),
              NULL,
              openssl_cipher->cipher_base.key.buffer,
              openssl_cipher->cipher_base.iv.buffer) &&
          EVP_CIPHER_CTX_set_padding(openssl_cipher->decryptor_ctx, 0))) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return AWS_OP_SUCCESS;
}

static int s_reset_ctr_cipher_materials(struct aws_symmetric_cipher *cipher) {
    int ret_val = s_clear_reusable_state(cipher);

    if (ret_val == AWS_OP_SUCCESS) {
        return s_init_ctr_cipher_materials(cipher);
    }

    return ret_val;
}

static struct aws_symmetric_cipher_vtable s_ctr_vtable = {
    .alg_name = "AES-CTR 256",
    .provider = "OpenSSL Compatible LibCrypto",
    .destroy = s_destroy,
    .reset = s_reset_ctr_cipher_materials,
    .decrypt = s_decrypt,
    .encrypt = s_encrypt,
    .finalize_decryption = s_finalize_decryption,
    .finalize_encryption = s_finalize_encryption,
};

struct aws_symmetric_cipher *aws_aes_ctr_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv) {
    struct openssl_aes_cipher *cipher = aws_mem_calloc(allocator, 1, sizeof(struct openssl_aes_cipher));

    cipher->cipher_base.allocator = allocator;
    cipher->cipher_base.block_size = AWS_AES_256_CIPHER_BLOCK_SIZE;
    cipher->cipher_base.key_length_bits = AWS_AES_256_KEY_BIT_LEN;
    cipher->cipher_base.vtable = &s_ctr_vtable;
    cipher->cipher_base.impl = cipher;

    if (key) {
        aws_byte_buf_init_copy_from_cursor(&cipher->cipher_base.key, allocator, *key);
    } else {
        aws_byte_buf_init(&cipher->cipher_base.key, allocator, AWS_AES_256_KEY_BYTE_LEN);
        aws_symmetric_cipher_generate_key(AWS_AES_256_KEY_BYTE_LEN, &cipher->cipher_base.key);
    }

    if (iv) {
        aws_byte_buf_init_copy_from_cursor(&cipher->cipher_base.iv, allocator, *iv);
    } else {
        aws_byte_buf_init(&cipher->cipher_base.iv, allocator, AWS_AES_256_CIPHER_BLOCK_SIZE);
        aws_symmetric_cipher_generate_initialization_vector(
            AWS_AES_256_CIPHER_BLOCK_SIZE, true, &cipher->cipher_base.iv);
    }

    /* EVP_CIPHER_CTX_init() will be called inside EVP_CIPHER_CTX_new(). */
    cipher->encryptor_ctx = EVP_CIPHER_CTX_new();
    AWS_FATAL_ASSERT(cipher->encryptor_ctx && "Cipher initialization failed!");

    /* EVP_CIPHER_CTX_init() will be called inside EVP_CIPHER_CTX_new(). */
    cipher->decryptor_ctx = EVP_CIPHER_CTX_new();
    AWS_FATAL_ASSERT(cipher->decryptor_ctx && "Cipher initialization failed!");

    if (s_init_ctr_cipher_materials(&cipher->cipher_base) != AWS_OP_SUCCESS) {
        goto error;
    }

    cipher->cipher_base.state = AWS_SYMMETRIC_CIPHER_READY;
    return &cipher->cipher_base;

error:
    s_destroy(&cipher->cipher_base);
    return NULL;
}

static int s_gcm_decrypt(struct aws_symmetric_cipher *cipher, struct aws_byte_cursor input, struct aws_byte_buf *out) {
    if (cipher->tag.buffer == NULL) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return s_decrypt(cipher, input, out);
}

static int s_finalize_gcm_encryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (cipher->tag.buffer == NULL) {
        aws_byte_buf_init(&cipher->tag, cipher->allocator, AWS_AES_256_CIPHER_BLOCK_SIZE);
    }

    int ret_val = s_finalize_encryption(cipher, out);

    if (ret_val == AWS_OP_SUCCESS) {
        if (!EVP_CIPHER_CTX_ctrl(
                openssl_cipher->encryptor_ctx, EVP_CTRL_GCM_GET_TAG, (int)cipher->tag.capacity, cipher->tag.buffer)) {
            cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
        cipher->tag.len = AWS_AES_256_CIPHER_BLOCK_SIZE;
    }

    return ret_val;
}

static int s_finalize_gcm_decryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (openssl_cipher->cipher_base.tag.len) {
        if (!EVP_CIPHER_CTX_ctrl(
                openssl_cipher->decryptor_ctx,
                EVP_CTRL_GCM_SET_TAG,
                (int)openssl_cipher->cipher_base.tag.len,
                openssl_cipher->cipher_base.tag.buffer)) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
    }

    return s_finalize_decryption(cipher, out);
}

static int s_init_gcm_cipher_materials(struct aws_symmetric_cipher *cipher) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (!(EVP_EncryptInit_ex(openssl_cipher->encryptor_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) &&
          EVP_EncryptInit_ex(
              openssl_cipher->encryptor_ctx,
              NULL,
              NULL,
              openssl_cipher->cipher_base.key.buffer,
              openssl_cipher->cipher_base.iv.buffer) &&
          EVP_CIPHER_CTX_set_padding(openssl_cipher->encryptor_ctx, 0)) ||
        !(EVP_DecryptInit_ex(openssl_cipher->decryptor_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) &&
          EVP_DecryptInit_ex(
              openssl_cipher->decryptor_ctx,
              NULL,
              NULL,
              openssl_cipher->cipher_base.key.buffer,
              openssl_cipher->cipher_base.iv.buffer) &&
          EVP_CIPHER_CTX_set_padding(openssl_cipher->decryptor_ctx, 0))) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (openssl_cipher->cipher_base.aad.len) {
        int outLen = 0;
        if (!EVP_EncryptUpdate(
                openssl_cipher->encryptor_ctx,
                NULL,
                &outLen,
                openssl_cipher->cipher_base.aad.buffer,
                (int)openssl_cipher->cipher_base.aad.len) ||
            !EVP_DecryptUpdate(
                openssl_cipher->decryptor_ctx,
                NULL,
                &outLen,
                openssl_cipher->cipher_base.aad.buffer,
                (int)openssl_cipher->cipher_base.aad.len)) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
    }

    aws_byte_buf_clean_up_secure(&openssl_cipher->cipher_base.tag);

    return AWS_OP_SUCCESS;
}

static int s_reset_gcm_cipher_materials(struct aws_symmetric_cipher *cipher) {
    int ret_val = s_clear_reusable_state(cipher);

    if (ret_val == AWS_OP_SUCCESS) {
        return s_init_gcm_cipher_materials(cipher);
    }

    return ret_val;
}

static struct aws_symmetric_cipher_vtable s_gcm_vtable = {
    .alg_name = "AES-GCM 256",
    .provider = "OpenSSL Compatible LibCrypto",
    .destroy = s_destroy,
    .reset = s_reset_gcm_cipher_materials,
    .decrypt = s_gcm_decrypt,
    .encrypt = s_encrypt,
    .finalize_decryption = s_finalize_gcm_decryption,
    .finalize_encryption = s_finalize_gcm_encryption,
};

struct aws_symmetric_cipher *aws_aes_gcm_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key,
    const struct aws_byte_cursor *iv,
    const struct aws_byte_cursor *aad) {

    struct openssl_aes_cipher *cipher = aws_mem_calloc(allocator, 1, sizeof(struct openssl_aes_cipher));
    cipher->cipher_base.allocator = allocator;
    cipher->cipher_base.block_size = AWS_AES_256_CIPHER_BLOCK_SIZE;
    cipher->cipher_base.key_length_bits = AWS_AES_256_KEY_BIT_LEN;
    cipher->cipher_base.vtable = &s_gcm_vtable;
    cipher->cipher_base.impl = cipher;

    /* Copy key into the cipher context. */
    if (key) {
        aws_byte_buf_init_copy_from_cursor(&cipher->cipher_base.key, allocator, *key);
    } else {
        aws_byte_buf_init(&cipher->cipher_base.key, allocator, AWS_AES_256_KEY_BYTE_LEN);
        aws_symmetric_cipher_generate_key(AWS_AES_256_KEY_BYTE_LEN, &cipher->cipher_base.key);
    }

    /* Copy initialization vector into the cipher context. */
    if (iv) {
        aws_byte_buf_init_copy_from_cursor(&cipher->cipher_base.iv, allocator, *iv);
    } else {
        aws_byte_buf_init(&cipher->cipher_base.iv, allocator, AWS_AES_256_CIPHER_BLOCK_SIZE - 4);
        aws_symmetric_cipher_generate_initialization_vector(
            AWS_AES_256_CIPHER_BLOCK_SIZE - 4, false, &cipher->cipher_base.iv);
    }

    /* Initialize the cipher contexts. */
    cipher->encryptor_ctx = EVP_CIPHER_CTX_new();
    AWS_FATAL_ASSERT(cipher->encryptor_ctx && "Encryptor cipher initialization failed!");

    cipher->decryptor_ctx = EVP_CIPHER_CTX_new();
    AWS_FATAL_ASSERT(cipher->decryptor_ctx && "Decryptor cipher initialization failed!");

    /* Set AAD if provided */
    if (aad) {
        aws_byte_buf_init_copy_from_cursor(&cipher->cipher_base.aad, allocator, *aad);
    }

    /* Initialize the cipher contexts with the specified key and IV. */
    if (s_init_gcm_cipher_materials(&cipher->cipher_base)) {
        goto error;
    }

    cipher->cipher_base.state = AWS_SYMMETRIC_CIPHER_READY;
    return &cipher->cipher_base;

error:
    s_destroy(&cipher->cipher_base);
    return NULL;
}

static int s_key_wrap_encrypt_decrypt(
    struct aws_symmetric_cipher *cipher,
    struct aws_byte_cursor input,
    struct aws_byte_buf *out) {
    (void)out;
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    return aws_byte_buf_append_dynamic(&openssl_cipher->working_buffer, &input);
}

static const size_t MIN_CEK_LENGTH_BYTES = 128 / 8;
static const unsigned char INTEGRITY_VALUE = 0xA6;
#define KEYWRAP_BLOCK_SIZE 8u

static int s_key_wrap_finalize_encryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (openssl_cipher->working_buffer.len < MIN_CEK_LENGTH_BYTES) {
        cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    /* the following is an in place implementation of
       RFC 3394 using the alternate in-place implementation.
       we use one in-place buffer instead of the copy at the end.
       the one letter variable names are meant to directly reflect the variables in the RFC */
    size_t required_buffer_space = openssl_cipher->working_buffer.len + cipher->block_size;
    size_t starting_len_offset = out->len;

    if (aws_symmetric_cipher_try_ensure_sufficient_buffer_space(out, required_buffer_space)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* put the integrity check register in the first 8 bytes of the final buffer. */
    aws_byte_buf_write_u8_n(out, INTEGRITY_VALUE, KEYWRAP_BLOCK_SIZE);
    uint8_t *a = out->buffer + starting_len_offset;

    struct aws_byte_cursor working_buf_cur = aws_byte_cursor_from_buf(&openssl_cipher->working_buffer);
    aws_byte_buf_write_from_whole_cursor(out, working_buf_cur);

    /* put the register buffer after the integrity check register */
    uint8_t *r = out->buffer + starting_len_offset + KEYWRAP_BLOCK_SIZE;

    int n = (int)(openssl_cipher->working_buffer.len / KEYWRAP_BLOCK_SIZE);

    uint8_t b_buf[KEYWRAP_BLOCK_SIZE * 2] = {0};
    struct aws_byte_buf b = aws_byte_buf_from_empty_array(b_buf, sizeof(b_buf));
    int b_out_len = b.capacity;

    uint8_t temp_buf[KEYWRAP_BLOCK_SIZE * 2] = {0};
    struct aws_byte_buf temp_input = aws_byte_buf_from_empty_array(temp_buf, sizeof(temp_buf));

    for (int j = 0; j <= 5; ++j) {
        for (int i = 1; i <= n; ++i) {
            /* concat A and R[i], A should be most significant and then R[i] should be least significant. */
            memcpy(temp_input.buffer, a, KEYWRAP_BLOCK_SIZE);
            memcpy(temp_input.buffer + KEYWRAP_BLOCK_SIZE, r, KEYWRAP_BLOCK_SIZE);

            /* encrypt the concatenated A and R[I] and store it in B */
            if (!EVP_EncryptUpdate(
                    openssl_cipher->encryptor_ctx, b.buffer, &b_out_len, temp_input.buffer, (int)temp_input.capacity)) {
                cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
                return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            }

            unsigned char t = (unsigned char)((n * j) + i);
            /* put the 64 MSB ^ T into A */
            memcpy(a, b.buffer, KEYWRAP_BLOCK_SIZE);
            a[7] ^= t;

            /* put the 64 LSB into R[i] */
            memcpy(r, b.buffer + KEYWRAP_BLOCK_SIZE, KEYWRAP_BLOCK_SIZE);
            /* increment i -> R[i] */
            r += KEYWRAP_BLOCK_SIZE;
        }
        /* reset R */
        r = out->buffer + starting_len_offset + KEYWRAP_BLOCK_SIZE;
    }

    return AWS_OP_SUCCESS;
}

static int s_key_wrap_finalize_decryption(struct aws_symmetric_cipher *cipher, struct aws_byte_buf *out) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (openssl_cipher->working_buffer.len < MIN_CEK_LENGTH_BYTES + KEYWRAP_BLOCK_SIZE) {
        cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    /* the following is an in place implementation of
       RFC 3394 using the alternate in-place implementation.
       we use one in-place buffer instead of the copy at the end.
       the one letter variable names are meant to directly reflect the variables in the RFC */
    size_t required_buffer_space = openssl_cipher->working_buffer.len - KEYWRAP_BLOCK_SIZE;
    size_t starting_len_offset = out->len;

    if (aws_symmetric_cipher_try_ensure_sufficient_buffer_space(out, required_buffer_space)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    memcpy(
        out->buffer + starting_len_offset,
        openssl_cipher->working_buffer.buffer + KEYWRAP_BLOCK_SIZE,
        required_buffer_space);

    /* integrity register should be the first 8 bytes of the final buffer. */
    uint8_t *a = openssl_cipher->working_buffer.buffer;

    /* in-place register is the plaintext. For decryption, start at the last array position (8 bytes before the end); */
    uint8_t *r = out->buffer + starting_len_offset + required_buffer_space - KEYWRAP_BLOCK_SIZE;

    int n = (int)(required_buffer_space / KEYWRAP_BLOCK_SIZE);

    uint8_t b_buf[KEYWRAP_BLOCK_SIZE * 10] = {0};
    struct aws_byte_buf b = aws_byte_buf_from_empty_array(b_buf, sizeof(b_buf));
    int b_out_len = b.capacity;

    uint8_t temp_buf[KEYWRAP_BLOCK_SIZE * 2] = {0};
    struct aws_byte_buf temp_input = aws_byte_buf_from_empty_array(temp_buf, sizeof(temp_buf));

    for (int j = 5; j >= 0; --j) {
        for (int i = n; i >= 1; --i) {
            /* concat A and T */
            memcpy(temp_input.buffer, a, KEYWRAP_BLOCK_SIZE);
            unsigned char t = (unsigned char)((n * j) + i);
            temp_input.buffer[7] ^= t;
            /* R[i] */
            memcpy(temp_input.buffer + KEYWRAP_BLOCK_SIZE, r, KEYWRAP_BLOCK_SIZE);

            /* Decrypt the concatenated buffer */
            if (!EVP_DecryptUpdate(
                    openssl_cipher->decryptor_ctx, b.buffer, &b_out_len, temp_input.buffer, (int)temp_input.capacity)) {
                cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
                return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            }

            /* set A to 64 MSB of decrypted result */
            memcpy(a, b.buffer, KEYWRAP_BLOCK_SIZE);
            /* set the R[i] to the 64 LSB of decrypted result */
            memcpy(r, b.buffer + KEYWRAP_BLOCK_SIZE, KEYWRAP_BLOCK_SIZE);
            /* decrement i -> R[i] */
            r -= KEYWRAP_BLOCK_SIZE;
        }
        /* reset R */
        r = out->buffer + starting_len_offset + required_buffer_space - KEYWRAP_BLOCK_SIZE;
    }

    /* here we perform the integrity check to make sure A == 0xA6A6A6A6A6A6A6A6 */
    for (size_t i = 0; i < KEYWRAP_BLOCK_SIZE; ++i) {
        if (a[i] != INTEGRITY_VALUE) {
            cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
            return aws_raise_error(AWS_ERROR_CAL_SIGNATURE_VALIDATION_FAILED);
        }
    }

    out->len += required_buffer_space;
    return AWS_OP_SUCCESS;
}

static int s_init_keywrap_cipher_materials(struct aws_symmetric_cipher *cipher) {
    struct openssl_aes_cipher *openssl_cipher = cipher->impl;

    if (!(EVP_EncryptInit_ex(openssl_cipher->encryptor_ctx, EVP_aes_256_ecb(), NULL, cipher->key.buffer, NULL) &&
          EVP_CIPHER_CTX_set_padding(openssl_cipher->encryptor_ctx, 0)) ||
        !(EVP_DecryptInit_ex(openssl_cipher->decryptor_ctx, EVP_aes_256_ecb(), NULL, cipher->key.buffer, NULL) &&
          EVP_CIPHER_CTX_set_padding(openssl_cipher->decryptor_ctx, 0))) {
        cipher->state = AWS_SYMMETRIC_CIPHER_ERROR;
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return AWS_OP_SUCCESS;
}

static int s_reset_keywrap_cipher_materials(struct aws_symmetric_cipher *cipher) {
    int ret_val = s_clear_reusable_state(cipher);

    if (ret_val == AWS_OP_SUCCESS) {
        return s_init_keywrap_cipher_materials(cipher);
    }

    return ret_val;
}

static struct aws_symmetric_cipher_vtable s_keywrap_vtable = {
    .alg_name = "AES-KEYWRAP 256",
    .provider = "OpenSSL Compatible LibCrypto",
    .destroy = s_destroy,
    .reset = s_reset_keywrap_cipher_materials,
    .decrypt = s_key_wrap_encrypt_decrypt,
    .encrypt = s_key_wrap_encrypt_decrypt,
    .finalize_decryption = s_key_wrap_finalize_decryption,
    .finalize_encryption = s_key_wrap_finalize_encryption,
};

struct aws_symmetric_cipher *aws_aes_keywrap_256_new_impl(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *key) {
    struct openssl_aes_cipher *cipher = aws_mem_calloc(allocator, 1, sizeof(struct openssl_aes_cipher));
    cipher->cipher_base.allocator = allocator;
    cipher->cipher_base.block_size = KEYWRAP_BLOCK_SIZE;
    cipher->cipher_base.key_length_bits = AWS_AES_256_KEY_BIT_LEN;
    cipher->cipher_base.vtable = &s_keywrap_vtable;
    cipher->cipher_base.impl = cipher;

    /* Copy key into the cipher context. */
    if (key) {
        aws_byte_buf_init_copy_from_cursor(&cipher->cipher_base.key, allocator, *key);
    } else {
        aws_byte_buf_init(&cipher->cipher_base.key, allocator, AWS_AES_256_KEY_BYTE_LEN);
        aws_symmetric_cipher_generate_key(AWS_AES_256_KEY_BYTE_LEN, &cipher->cipher_base.key);
    }

    aws_byte_buf_init(&cipher->working_buffer, allocator, KEYWRAP_BLOCK_SIZE);

    /* Initialize the cipher contexts. */
    cipher->encryptor_ctx = EVP_CIPHER_CTX_new();
    AWS_FATAL_ASSERT(cipher->encryptor_ctx && "Encryptor cipher initialization failed!");

    cipher->decryptor_ctx = EVP_CIPHER_CTX_new();
    AWS_FATAL_ASSERT(cipher->decryptor_ctx && "Decryptor cipher initialization failed!");

    /* Initialize the cipher contexts with the specified key and IV. */
    if (s_init_keywrap_cipher_materials(&cipher->cipher_base)) {
        goto error;
    }

    cipher->cipher_base.state = AWS_SYMMETRIC_CIPHER_READY;
    return &cipher->cipher_base;

error:
    s_destroy(&cipher->cipher_base);
    return NULL;
}
