/*
 * Copyright 2019-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Comments in this implementation refer to:
 * [MCGREW] https://tools.ietf.org/html/draft-mcgrew-aead-aes-cbc-hmac-sha2-05
 */

#include <bson/bson.h>

#include "mongocrypt-binary-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-log-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-status-private.h"

#include <inttypes.h>

/* This function uses ECB callback to simulate CTR encrypt and decrypt
 *
 * Note: the same function performs both encrypt and decrypt using same ECB
 * encryption function
 */

static bool _crypto_aes_256_ctr_encrypt_decrypt_via_ecb(void *ctx,
                                                        mongocrypt_crypto_fn aes_256_ecb_encrypt,
                                                        aes_256_args_t args,
                                                        mongocrypt_status_t *status) {
    BSON_ASSERT(args.iv && args.iv->len);
    BSON_ASSERT(args.in);
    BSON_ASSERT(args.out);

    if (args.out->len < args.in->len) {
        CLIENT_ERR("output buffer too small");
        return false;
    }

    _mongocrypt_buffer_t ctr, tmp;
    mongocrypt_binary_t key_bin, out_bin, in_bin, ctr_bin, tmp_bin;
    bool ret;

    _mongocrypt_buffer_to_binary(args.key, &key_bin);
    _mongocrypt_buffer_init(&ctr);
    _mongocrypt_buffer_copy_to(args.iv, &ctr);
    _mongocrypt_buffer_to_binary(&ctr, &ctr_bin);
    _mongocrypt_buffer_to_binary(args.out, &out_bin);
    _mongocrypt_buffer_to_binary(args.in, &in_bin);
    _mongocrypt_buffer_init_size(&tmp, args.iv->len);
    _mongocrypt_buffer_to_binary(&tmp, &tmp_bin);

    for (uint32_t ptr = 0; ptr < args.in->len;) {
        /* Encrypt value in CTR buffer */
        uint32_t bytes_written = 0;
        if (!aes_256_ecb_encrypt(ctx, &key_bin, NULL, &ctr_bin, &tmp_bin, &bytes_written, status)) {
            ret = false;
            goto cleanup;
        }

        if (bytes_written != tmp_bin.len) {
            CLIENT_ERR("encryption hook returned unexpected length");
            ret = false;
            goto cleanup;
        }

        /* XOR resulting stream with original data */
        for (uint32_t i = 0; i < bytes_written && ptr < args.in->len; i++, ptr++) {
            out_bin.data[ptr] = in_bin.data[ptr] ^ tmp_bin.data[i];
        }

        /* Increment value in CTR buffer */
        uint32_t carry = 1;
        /* assert rather than return since this should never happen */
        BSON_ASSERT(ctr_bin.len == 0u || ctr_bin.len - 1u <= INT_MAX);
        for (int i = (int)ctr_bin.len - 1; i >= 0 && carry != 0; --i) {
            uint32_t bpp = carry + ctr_bin.data[i];
            carry = bpp >> 8;
            ctr_bin.data[i] = bpp & 0xFF;
        }
    }

    if (args.bytes_written) {
        *args.bytes_written = args.in->len;
    }

    ret = true;

cleanup:
    _mongocrypt_buffer_cleanup(&ctr);
    _mongocrypt_buffer_cleanup(&tmp);
    return ret;
}

/* Crypto primitives. These either call the native built in crypto primitives or
 * user supplied hooks. */
static bool _crypto_aes_256_cbc_encrypt(_mongocrypt_crypto_t *crypto, aes_256_args_t args) {
    mongocrypt_status_t *status = args.status;

    BSON_ASSERT_PARAM(crypto);

    BSON_ASSERT(args.key);
    if (args.key->len != MONGOCRYPT_ENC_KEY_LEN) {
        CLIENT_ERR("invalid encryption key length");
        return false;
    }

    BSON_ASSERT(args.iv);
    if (args.iv->len != MONGOCRYPT_IV_LEN) {
        CLIENT_ERR("invalid iv length");
        return false;
    }

    if (crypto->hooks_enabled) {
        mongocrypt_binary_t enc_key_bin, iv_bin, out_bin, in_bin;
        bool ret;

        _mongocrypt_buffer_to_binary(args.key, &enc_key_bin);
        _mongocrypt_buffer_to_binary(args.iv, &iv_bin);
        _mongocrypt_buffer_to_binary(args.out, &out_bin);
        _mongocrypt_buffer_to_binary(args.in, &in_bin);

        ret = crypto->aes_256_cbc_encrypt(crypto->ctx,
                                          &enc_key_bin,
                                          &iv_bin,
                                          &in_bin,
                                          &out_bin,
                                          args.bytes_written,
                                          status);
        return ret;
    }
    return _native_crypto_aes_256_cbc_encrypt(args);
}

static bool _crypto_aes_256_ctr_encrypt(_mongocrypt_crypto_t *crypto, aes_256_args_t args) {
    mongocrypt_status_t *status = args.status;

    BSON_ASSERT_PARAM(crypto);

    BSON_ASSERT(args.key);
    if (args.key->len != MONGOCRYPT_ENC_KEY_LEN) {
        CLIENT_ERR("invalid encryption key length");
        return false;
    }

    BSON_ASSERT(args.iv);
    if (args.iv->len != MONGOCRYPT_IV_LEN) {
        CLIENT_ERR("invalid iv length");
        return false;
    }

    if (crypto->aes_256_ctr_encrypt) {
        mongocrypt_binary_t enc_key_bin, iv_bin, out_bin, in_bin;
        bool ret;

        _mongocrypt_buffer_to_binary(args.key, &enc_key_bin);
        _mongocrypt_buffer_to_binary(args.iv, &iv_bin);
        _mongocrypt_buffer_to_binary(args.out, &out_bin);
        _mongocrypt_buffer_to_binary(args.in, &in_bin);

        ret = crypto->aes_256_ctr_encrypt(crypto->ctx,
                                          &enc_key_bin,
                                          &iv_bin,
                                          &in_bin,
                                          &out_bin,
                                          args.bytes_written,
                                          status);
        return ret;
    }

    if (crypto->aes_256_ecb_encrypt) {
        return _crypto_aes_256_ctr_encrypt_decrypt_via_ecb(crypto->ctx, crypto->aes_256_ecb_encrypt, args, status);
    }

    return _native_crypto_aes_256_ctr_encrypt(args);
}

static bool _crypto_aes_256_cbc_decrypt(_mongocrypt_crypto_t *crypto, aes_256_args_t args) {
    mongocrypt_status_t *status = args.status;

    BSON_ASSERT_PARAM(crypto);

    BSON_ASSERT(args.key);
    if (args.key->len != MONGOCRYPT_ENC_KEY_LEN) {
        CLIENT_ERR("invalid encryption key length");
        return false;
    }

    if (crypto->hooks_enabled) {
        mongocrypt_binary_t enc_key_bin, iv_bin, out_bin, in_bin;
        bool ret;

        _mongocrypt_buffer_to_binary(args.key, &enc_key_bin);
        _mongocrypt_buffer_to_binary(args.iv, &iv_bin);
        _mongocrypt_buffer_to_binary(args.out, &out_bin);
        _mongocrypt_buffer_to_binary(args.in, &in_bin);

        ret = crypto->aes_256_cbc_decrypt(crypto->ctx,
                                          &enc_key_bin,
                                          &iv_bin,
                                          &in_bin,
                                          &out_bin,
                                          args.bytes_written,
                                          status);
        return ret;
    }
    return _native_crypto_aes_256_cbc_decrypt(args);
}

static bool _crypto_aes_256_ctr_decrypt(_mongocrypt_crypto_t *crypto, aes_256_args_t args) {
    mongocrypt_status_t *status = args.status;

    BSON_ASSERT_PARAM(crypto);

    BSON_ASSERT(args.key);
    if (args.key->len != MONGOCRYPT_ENC_KEY_LEN) {
        CLIENT_ERR("invalid encryption key length");
        return false;
    }

    if (crypto->aes_256_ctr_decrypt) {
        mongocrypt_binary_t enc_key_bin, iv_bin, out_bin, in_bin;
        bool ret;

        _mongocrypt_buffer_to_binary(args.key, &enc_key_bin);
        _mongocrypt_buffer_to_binary(args.iv, &iv_bin);
        _mongocrypt_buffer_to_binary(args.out, &out_bin);
        _mongocrypt_buffer_to_binary(args.in, &in_bin);

        ret = crypto->aes_256_ctr_decrypt(crypto->ctx,
                                          &enc_key_bin,
                                          &iv_bin,
                                          &in_bin,
                                          &out_bin,
                                          args.bytes_written,
                                          status);
        return ret;
    }

    if (crypto->aes_256_ecb_encrypt) {
        return _crypto_aes_256_ctr_encrypt_decrypt_via_ecb(crypto->ctx, crypto->aes_256_ecb_encrypt, args, status);
    }

    return _native_crypto_aes_256_ctr_decrypt(args);
}

static bool _crypto_hmac_sha_512(_mongocrypt_crypto_t *crypto,
                                 const _mongocrypt_buffer_t *hmac_key,
                                 const _mongocrypt_buffer_t *in,
                                 _mongocrypt_buffer_t *out,
                                 mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(hmac_key);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    if (hmac_key->len != MONGOCRYPT_MAC_KEY_LEN) {
        CLIENT_ERR("invalid hmac key length");
        return false;
    }

    if (out->len != MONGOCRYPT_HMAC_SHA512_LEN) {
        CLIENT_ERR("out does not contain %d bytes", MONGOCRYPT_HMAC_SHA512_LEN);
        return false;
    }

    if (crypto->hooks_enabled) {
        mongocrypt_binary_t hmac_key_bin, out_bin, in_bin;
        bool ret;

        _mongocrypt_buffer_to_binary(hmac_key, &hmac_key_bin);
        _mongocrypt_buffer_to_binary(out, &out_bin);
        _mongocrypt_buffer_to_binary(in, &in_bin);

        ret = crypto->hmac_sha_512(crypto->ctx, &hmac_key_bin, &in_bin, &out_bin, status);
        return ret;
    }
    return _native_crypto_hmac_sha_512(hmac_key, in, out, status);
}

bool _mongocrypt_hmac_sha_256(_mongocrypt_crypto_t *crypto,
                              const _mongocrypt_buffer_t *key,
                              const _mongocrypt_buffer_t *in,
                              _mongocrypt_buffer_t *out,
                              mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    if (key->len != MONGOCRYPT_MAC_KEY_LEN) {
        CLIENT_ERR("invalid hmac_sha_256 key length. Got %" PRIu32 ", expected: %" PRIu32,
                   key->len,
                   MONGOCRYPT_MAC_KEY_LEN);
        return false;
    }

    if (crypto->hooks_enabled) {
        mongocrypt_binary_t key_bin, out_bin, in_bin;
        _mongocrypt_buffer_to_binary(key, &key_bin);
        _mongocrypt_buffer_to_binary(out, &out_bin);
        _mongocrypt_buffer_to_binary(in, &in_bin);

        return crypto->hmac_sha_256(crypto->ctx, &key_bin, &in_bin, &out_bin, status);
    }
    return _native_crypto_hmac_sha_256(key, in, out, status);
}

static bool
_crypto_random(_mongocrypt_crypto_t *crypto, _mongocrypt_buffer_t *out, uint32_t count, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(out);

    if (out->len != count) {
        CLIENT_ERR("out does not contain %u bytes", count);
        return false;
    }

    if (crypto->hooks_enabled) {
        mongocrypt_binary_t out_bin;

        _mongocrypt_buffer_to_binary(out, &out_bin);
        return crypto->random(crypto->ctx, &out_bin, count, status);
    }
    return _native_crypto_random(out, count, status);
}

/*
 * Secure memcmp copied from the C driver.
 */
int _mongocrypt_memequal(const void *const b1, const void *const b2, size_t len) {
    const unsigned char *p1 = b1, *p2 = b2;
    int ret = 0;

    BSON_ASSERT_PARAM(b1);
    BSON_ASSERT_PARAM(b2);

    for (; len > 0; len--) {
        ret |= *p1++ ^ *p2++;
    }

    return ret;
}

typedef enum {
    MODE_CBC,
    MODE_CTR,
} _mongocrypt_encryption_mode_t;

typedef enum {
    HMAC_NONE,
    HMAC_SHA_512_256, // sha512 truncated to 256 bits
    HMAC_SHA_256,
} _mongocrypt_hmac_type_t;

typedef enum {
    KEY_FORMAT_FLE1,       // 32 octets MAC key, 32 DATA key, 32 IV key (ignored)
    KEY_FORMAT_FLE2,       // 32 octets DATA key
    KEY_FORMAT_FLE2AEAD,   // 32 octets DATA key, 32 MAC key, 32 IV key (ignored)
    KEY_FORMAT_FLE2v2AEAD, // 32 octets DATA key, 32 MAC key, 32 IV key (ignored)
} _mongocrypt_key_format_t;

typedef enum {
    MAC_FORMAT_FLE1,       // HMAC(AAD || IV || S || LEN(AAD) as uint64be)
    MAC_FORMAT_FLE2,       // NONE
    MAC_FORMAT_FLE2AEAD,   // HMAC(AAD || IV || S)
    MAC_FORMAT_FLE2v2AEAD, // HMAC(AAD || IV || S)
} _mongocrypt_mac_format_t;

/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_calculate_ciphertext_len
 *
 * Calculate the space needed for a ciphertext payload of a given size
 * and using fixed iv/hmac lengths.
 *
 * MODE_CBC: Assumes the ciphertext will be padded according to PKCS#7
 * which rounds up to the next block size, adding up to a complete block
 * for block aligned input payloads.
 *
 * MODE_CTR: Assumes no additional padding since CTR is a streaming cipher.
 *
 * Assumes all algorithms use identical IV length and blocksizes.
 *
 * ----------------------------------------------------------------------------
 */
static uint32_t _mongocrypt_calculate_ciphertext_len(uint32_t inlen,
                                                     _mongocrypt_encryption_mode_t mode,
                                                     _mongocrypt_hmac_type_t hmac,
                                                     mongocrypt_status_t *status) {
    const uint32_t hmaclen = (hmac == HMAC_NONE) ? 0 : MONGOCRYPT_HMAC_LEN;
    const uint32_t maxinlen = UINT32_MAX - (MONGOCRYPT_IV_LEN + MONGOCRYPT_BLOCK_SIZE + hmaclen);
    uint32_t fill;
    if (inlen > maxinlen) {
        CLIENT_ERR("plaintext too long");
        return 0;
    }

    if (mode == MODE_CBC) {
        fill = MONGOCRYPT_BLOCK_SIZE - (inlen % MONGOCRYPT_BLOCK_SIZE);
    } else {
        BSON_ASSERT(mode == MODE_CTR);
        fill = 0;
    }

    return MONGOCRYPT_IV_LEN + inlen + fill + hmaclen;
}

/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_calculate_plaintext_len
 *
 * Calculate the space needed for a plaintext payload of a given size
 * and using fixed iv/hmac lengths.
 *
 * MODE_CBC: In practice, plaintext will be between 1 and {blocksize} bytes
 * shorter
 * than the input ciphertext, but it's easier and safer to assume the
 * full ciphertext length and waste a few bytes.
 *
 * MODE_CTR: Assumes no additional padding since CTR is a streaming cipher.
 *
 * Assumes all algorithms use identical IV length and blocksizes.
 *
 * ----------------------------------------------------------------------------
 */
static uint32_t _mongocrypt_calculate_plaintext_len(uint32_t inlen,
                                                    _mongocrypt_encryption_mode_t mode,
                                                    _mongocrypt_hmac_type_t hmac,
                                                    mongocrypt_status_t *status) {
    const uint32_t hmaclen = (hmac == HMAC_NONE) ? 0 : MONGOCRYPT_HMAC_LEN;
    const uint32_t mincipher = (mode == MODE_CTR) ? 0 : MONGOCRYPT_BLOCK_SIZE;
    if (inlen < (MONGOCRYPT_IV_LEN + mincipher + hmaclen)) {
        CLIENT_ERR("input ciphertext too small. Must be at least %" PRIu32 " bytes",
                   MONGOCRYPT_IV_LEN + mincipher + hmaclen);
        return 0;
    }
    return inlen - (MONGOCRYPT_IV_LEN + hmaclen);
}

/* ----------------------------------------------------------------------------
 *
 * _aes256_cbc_encrypt --
 *
 *    Encrypts using AES256 CBC using a secret key and a known IV.
 *
 * Parameters:
 *    @iv a 16 byte IV.
 *    @enc_key a 32 byte key.
 *    @plaintext the plaintext to encrypt.
 *    @ciphertext the resulting ciphertext.
 *    @bytes_written a location for the resulting number of bytes written into
 *    ciphertext->data.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 * Preconditions:
 *    1. ciphertext->data has been pre-allocated with enough space for the
 *    resulting ciphertext.
 *
 * Postconditions:
 *    1. bytes_written is set to the length of the written ciphertext. This
 *    is the same as
 *    _mongocrypt_calculate_ciphertext_len (plaintext->len, mode, hmac, status).
 *
 * ----------------------------------------------------------------------------
 */
static bool _encrypt_step(_mongocrypt_crypto_t *crypto,
                          _mongocrypt_encryption_mode_t mode,
                          const _mongocrypt_buffer_t *iv,
                          const _mongocrypt_buffer_t *enc_key,
                          const _mongocrypt_buffer_t *plaintext,
                          _mongocrypt_buffer_t *ciphertext,
                          uint32_t *bytes_written,
                          mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iv);
    BSON_ASSERT_PARAM(enc_key);
    BSON_ASSERT_PARAM(plaintext);
    BSON_ASSERT_PARAM(ciphertext);

    BSON_ASSERT_PARAM(bytes_written);
    *bytes_written = 0;

    if (MONGOCRYPT_IV_LEN != iv->len) {
        CLIENT_ERR("IV should have length %d, but has length %d", MONGOCRYPT_IV_LEN, iv->len);
        return false;
    }

    if (MONGOCRYPT_ENC_KEY_LEN != enc_key->len) {
        CLIENT_ERR("Encryption key should have length %d, but has length %d", MONGOCRYPT_ENC_KEY_LEN, enc_key->len);
        return false;
    }

    if (mode == MODE_CTR) {
        // Streaming cipher, no padding required.
        return _crypto_aes_256_ctr_encrypt(crypto,
                                           (aes_256_args_t){.key = enc_key,
                                                            .iv = iv,
                                                            .in = plaintext,
                                                            .out = ciphertext,
                                                            .bytes_written = bytes_written,
                                                            .status = status});
    }

    BSON_ASSERT(mode == MODE_CBC);

    /* calculate how many extra bytes there are after a block boundary */
    const uint32_t unaligned = plaintext->len % MONGOCRYPT_BLOCK_SIZE;
    uint32_t padding_byte = MONGOCRYPT_BLOCK_SIZE - unaligned;
    _mongocrypt_buffer_t intermediates[2], to_encrypt;
    uint8_t final_block_storage[MONGOCRYPT_BLOCK_SIZE];
    bool ret;

    BSON_ASSERT(MONGOCRYPT_BLOCK_SIZE >= unaligned);

    /* Some crypto providers disallow variable length inputs, and require
     * the input to be a multiple of the block size. So add everything up
     * to but excluding the last block if not block aligned, then add
     * the last block with padding. */
    _mongocrypt_buffer_init(&intermediates[0]);
    _mongocrypt_buffer_init(&intermediates[1]);
    intermediates[0].data = (uint8_t *)plaintext->data;
    /* don't check plaintext->len, as the above modulo operation guarantees
     * that unaligned will be smaller */
    intermediates[0].len = plaintext->len - unaligned;
    intermediates[1].data = final_block_storage;
    intermediates[1].len = sizeof(final_block_storage);

    /* [MCGREW]: "Prior to CBC encryption, the plaintext P is padded by appending
     * a padding string PS to that data, to ensure that len(P || PS) is a
     * multiple of 128". This is also known as PKCS #7 padding. */
    if (unaligned) {
        /* Copy the unaligned bytes. */
        memcpy(intermediates[1].data, plaintext->data + (plaintext->len - unaligned), unaligned);
    }
    /* Fill out block remained or whole block with padding_byte */
    memset(intermediates[1].data + unaligned, (int)padding_byte, padding_byte);

    _mongocrypt_buffer_init(&to_encrypt);
    if (!_mongocrypt_buffer_concat(&to_encrypt, intermediates, 2)) {
        CLIENT_ERR("failed to allocate buffer");
        _mongocrypt_buffer_cleanup(&to_encrypt);
        return false;
    }

    ret = _crypto_aes_256_cbc_encrypt(crypto,
                                      (aes_256_args_t){.key = enc_key,
                                                       .iv = iv,
                                                       .in = &to_encrypt,
                                                       .out = ciphertext,
                                                       .bytes_written = bytes_written,
                                                       .status = status});
    _mongocrypt_buffer_cleanup(&to_encrypt);
    if (!ret) {
        return false;
    }

    if (*bytes_written % MONGOCRYPT_BLOCK_SIZE != 0) {
        CLIENT_ERR("encryption failure, wrote %d bytes, not a multiple of %d", *bytes_written, MONGOCRYPT_BLOCK_SIZE);
        return false;
    }

    return true;
}

/* ----------------------------------------------------------------------------
 *
 * _hmac_step --
 *
 *    Compute the selected HMAC with a secret key.
 *
 * Parameters:
 *    @Km a 32 byte key.
 *    @AAD associated data to add into the HMAC. This may be
 *    an empty buffer.
 *    @iv_and_ciphertext the IV and S components to add into the HMAC.
 *    @out a location for the resulting HMAC tag.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 * Preconditions:
 *    1. out->data has been pre-allocated with at least 64 bytes.
 *
 * Postconditions:
 *    1. out->data will have a 64 byte tag appended.
 *
 * ----------------------------------------------------------------------------
 */
static bool _hmac_step(_mongocrypt_crypto_t *crypto,
                       _mongocrypt_mac_format_t mac_format,
                       _mongocrypt_hmac_type_t hmac,
                       const _mongocrypt_buffer_t *Km,
                       const _mongocrypt_buffer_t *AAD,
                       const _mongocrypt_buffer_t *iv_and_ciphertext,
                       _mongocrypt_buffer_t *out,
                       mongocrypt_status_t *status) {
    _mongocrypt_buffer_t to_hmac = {0};
    bool ret = false;

    BSON_ASSERT(hmac != HMAC_NONE);
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(Km);
    // AAD may be NULL
    BSON_ASSERT_PARAM(iv_and_ciphertext);
    BSON_ASSERT_PARAM(out);

    _mongocrypt_buffer_init(&to_hmac);

    if (MONGOCRYPT_MAC_KEY_LEN != Km->len) {
        CLIENT_ERR("HMAC key wrong length: %d", Km->len);
        goto done;
    }

    if (out->len != MONGOCRYPT_HMAC_LEN) {
        CLIENT_ERR("out wrong length: %d", out->len);
        goto done;
    }

    /* Construct the input to the HMAC */
    uint32_t num_intermediates = 0;
    _mongocrypt_buffer_t intermediates[3];
    if (AAD && !_mongocrypt_buffer_from_subrange(&intermediates[num_intermediates++], AAD, 0, AAD->len)) {
        CLIENT_ERR("Failed creating MAC subrange on AD");
        goto done;
    }
    if (!_mongocrypt_buffer_from_subrange(&intermediates[num_intermediates++],
                                          iv_and_ciphertext,
                                          0,
                                          iv_and_ciphertext->len)) {
        CLIENT_ERR("Failed creating MAC subrange on IV and S");
        goto done;
    }

    // {AL} must be stored in the function's lexical scope so that
    // {intermediates}'s reference to it survives until the
    // _mongocrypt_buffer_concat operation later.
    uint64_t AL;
    if (mac_format == MAC_FORMAT_FLE1) {
        /* T := HMAC(AAD || IV || S || AL)
         * AL is equal to the number of bits in AAD expressed
         * as a 64bit unsigned big-endian integer.
         * Multiplying a uint32_t by 8 won't bring it anywhere close to
         * UINT64_MAX.
         */
        AL = AAD ? BSON_UINT64_TO_BE(8 * (uint64_t)AAD->len) : 0;
        _mongocrypt_buffer_init(&intermediates[num_intermediates]);
        intermediates[num_intermediates].data = (uint8_t *)&AL;
        intermediates[num_intermediates++].len = sizeof(uint64_t);

    } else {
        /* T := HMAC(AAD || IV || S) */
        BSON_ASSERT((mac_format == MAC_FORMAT_FLE2AEAD) || (mac_format == MAC_FORMAT_FLE2v2AEAD));
    }

    if (!_mongocrypt_buffer_concat(&to_hmac, intermediates, num_intermediates)) {
        CLIENT_ERR("failed to allocate buffer");
        goto done;
    }

    if (hmac == HMAC_SHA_512_256) {
        uint8_t storage[64];
        _mongocrypt_buffer_t tag = {.data = storage, .len = sizeof(storage)};

        if (!_crypto_hmac_sha_512(crypto, Km, &to_hmac, &tag, status)) {
            goto done;
        }

        // Truncate sha512 to first 256 bits.
        memcpy(out->data, tag.data, MONGOCRYPT_HMAC_LEN);

    } else {
        BSON_ASSERT(hmac == HMAC_SHA_256);
        if (!_mongocrypt_hmac_sha_256(crypto, Km, &to_hmac, out, status)) {
            goto done;
        }
    }

    ret = true;
done:
    _mongocrypt_buffer_cleanup(&to_hmac);
    return ret;
}

/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_do_encryption --
 *
 *    Defer encryption to whichever crypto library libmongocrypt is using.
 *
 * Parameters:
 *    @iv a 16 byte IV.
 *    @associated_data associated data for the HMAC. May be NULL.
 *    @key is the encryption key. The size depends on @key_format.
 *    @plaintext the plaintext to encrypt.
 *    @ciphertext a location for the resulting ciphertext and HMAC tag.
 *    @bytes_written a location for the resulting bytes written.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 * Preconditions:
 *    1. ciphertext->data has been pre-allocated with enough space for the
 *    resulting ciphertext. Use _mongocrypt_calculate_ciphertext_len.
 *
 * Postconditions:
 *    1. bytes_written is set to the length of the written ciphertext. This
 *    is the same as
 *    _mongocrypt_calculate_ciphertext_len (plaintext->len, mode, hmac, status).
 *
 * ----------------------------------------------------------------------------
 */
static bool _mongocrypt_do_encryption(_mongocrypt_crypto_t *crypto,
                                      _mongocrypt_key_format_t key_format,
                                      _mongocrypt_mac_format_t mac_format,
                                      _mongocrypt_encryption_mode_t mode,
                                      _mongocrypt_hmac_type_t hmac,
                                      const _mongocrypt_buffer_t *iv,
                                      const _mongocrypt_buffer_t *associated_data,
                                      const _mongocrypt_buffer_t *key,
                                      const _mongocrypt_buffer_t *plaintext,
                                      _mongocrypt_buffer_t *ciphertext,
                                      uint32_t *bytes_written,
                                      mongocrypt_status_t *status) {
    _mongocrypt_buffer_t Ke = {0}; // Ke == Key for Encryption
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iv);
    /* associated_data is checked at the point it is used, so it can be NULL */
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(plaintext);
    BSON_ASSERT_PARAM(ciphertext);

    if (plaintext->len <= 0) {
        CLIENT_ERR("input plaintext too small. Must be more than zero bytes.");
        return false;
    }

    const uint32_t expect_ciphertext_len = _mongocrypt_calculate_ciphertext_len(plaintext->len, mode, hmac, status);
    if (mongocrypt_status_type(status) != MONGOCRYPT_STATUS_OK) {
        return false;
    }
    if (expect_ciphertext_len != ciphertext->len) {
        CLIENT_ERR("output ciphertext should have been allocated with %d bytes", expect_ciphertext_len);
        return false;
    }

    if (MONGOCRYPT_IV_LEN != iv->len) {
        CLIENT_ERR("IV should have length %d, but has length %d", MONGOCRYPT_IV_LEN, iv->len);
        return false;
    }

    const uint32_t expected_key_len = (key_format == KEY_FORMAT_FLE2) ? MONGOCRYPT_ENC_KEY_LEN : MONGOCRYPT_KEY_LEN;
    if (key->len != expected_key_len) {
        CLIENT_ERR("key should have length %d, but has length %d", expected_key_len, key->len);
        return false;
    }

    // Copy IV into the output, and clear remainder.
    memmove(ciphertext->data, iv->data, MONGOCRYPT_IV_LEN);
    memset(ciphertext->data + MONGOCRYPT_IV_LEN, 0, ciphertext->len - MONGOCRYPT_IV_LEN);

    // S is the encryption payload without IV or HMAC
    _mongocrypt_buffer_t S;
    if (!_mongocrypt_buffer_from_subrange(&S, ciphertext, MONGOCRYPT_IV_LEN, ciphertext->len - MONGOCRYPT_IV_LEN)) {
        CLIENT_ERR("unable to create S subrange from C");
        return false;
    }
    if (hmac != HMAC_NONE) {
        S.len -= MONGOCRYPT_HMAC_LEN;
    }

    // Ke is the key used for payload encryption
    const uint32_t Ke_offset = (key_format == KEY_FORMAT_FLE1) ? MONGOCRYPT_MAC_KEY_LEN : 0;
    if (!_mongocrypt_buffer_from_subrange(&Ke, key, Ke_offset, MONGOCRYPT_ENC_KEY_LEN)) {
        CLIENT_ERR("unable to create Ke subrange from key");
        return false;
    }

    uint32_t S_bytes_written = 0;
    if (!_encrypt_step(crypto, mode, iv, &Ke, plaintext, &S, &S_bytes_written, status)) {
        return false;
    }
    BSON_ASSERT_PARAM(bytes_written);
    BSON_ASSERT((UINT32_MAX - S_bytes_written) > MONGOCRYPT_IV_LEN);
    *bytes_written = MONGOCRYPT_IV_LEN + S_bytes_written;

    if (hmac != HMAC_NONE) {
        // Km == Key for MAC
        const uint32_t Km_offset = (key_format == KEY_FORMAT_FLE1) ? 0 : MONGOCRYPT_ENC_KEY_LEN;

        // Km is the HMAC Key.
        _mongocrypt_buffer_t Km;
        if (!_mongocrypt_buffer_from_subrange(&Km, key, Km_offset, MONGOCRYPT_MAC_KEY_LEN)) {
            CLIENT_ERR("unable to create Km subrange from key");
            return false;
        }

        /* Primary payload to MAC. */
        _mongocrypt_buffer_t iv_and_ciphertext;
        if (!_mongocrypt_buffer_from_subrange(&iv_and_ciphertext, ciphertext, 0, *bytes_written)) {
            CLIENT_ERR("unable to create IV || S subrange from C");
            return false;
        }

        // T == HMAC Tag
        _mongocrypt_buffer_t T;
        if (!_mongocrypt_buffer_from_subrange(&T, ciphertext, *bytes_written, MONGOCRYPT_HMAC_LEN)) {
            CLIENT_ERR("unable to create T subrange from C");
            return false;
        }

        if (!_hmac_step(crypto, mac_format, hmac, &Km, associated_data, &iv_and_ciphertext, &T, status)) {
            return false;
        }

        *bytes_written += MONGOCRYPT_HMAC_LEN;
    }

    return true;
}

/* ----------------------------------------------------------------------------
 *
 * _decrypt_step --
 *
 *    Decrypts using AES256 using a secret key and a known IV.
 *
 * Parameters:
 *    @enc_key a 32 byte key.
 *    @ciphertext the ciphertext to decrypt.
 *    @plaintext the resulting plaintext.
 *    @bytes_written a location for the resulting number of bytes written into
 *    plaintext->data.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 * Preconditions:
 *    1. plaintext->data has been pre-allocated with enough space for the
 *    resulting plaintext.
 *
 * Postconditions:
 *    1. bytes_written is set to the length of the written plaintext, excluding
 *    padding. This may be less than
 *    _mongocrypt_calculate_plaintext_len (ciphertext->len, home, hmac, status).
 *
 * ----------------------------------------------------------------------------
 */
static bool _decrypt_step(_mongocrypt_crypto_t *crypto,
                          _mongocrypt_encryption_mode_t mode,
                          const _mongocrypt_buffer_t *iv,
                          const _mongocrypt_buffer_t *enc_key,
                          const _mongocrypt_buffer_t *ciphertext,
                          _mongocrypt_buffer_t *plaintext,
                          uint32_t *bytes_written,
                          mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iv);
    BSON_ASSERT_PARAM(enc_key);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT_PARAM(plaintext);

    BSON_ASSERT_PARAM(bytes_written);
    *bytes_written = 0;

    if (MONGOCRYPT_IV_LEN != iv->len) {
        CLIENT_ERR("IV should have length %d, but has length %d", MONGOCRYPT_IV_LEN, iv->len);
        return false;
    }
    if (MONGOCRYPT_ENC_KEY_LEN != enc_key->len) {
        CLIENT_ERR("encryption key should have length %d, but has length %d", MONGOCRYPT_ENC_KEY_LEN, enc_key->len);
        return false;
    }

    if (mode == MODE_CBC) {
        if (ciphertext->len % MONGOCRYPT_BLOCK_SIZE > 0) {
            CLIENT_ERR("error, ciphertext length is not a multiple of block size");
            return false;
        }

        if (!_crypto_aes_256_cbc_decrypt(crypto,
                                         (aes_256_args_t){.iv = iv,
                                                          .key = enc_key,
                                                          .in = ciphertext,
                                                          .out = plaintext,
                                                          .bytes_written = bytes_written,
                                                          .status = status})) {
            return false;
        }

        BSON_ASSERT(*bytes_written > 0);
        uint8_t padding_byte = plaintext->data[*bytes_written - 1];
        if (padding_byte > 16) {
            CLIENT_ERR("error, ciphertext malformed padding");
            return false;
        }
        *bytes_written -= padding_byte;

    } else {
        BSON_ASSERT(mode == MODE_CTR);
        if (!_crypto_aes_256_ctr_decrypt(crypto,
                                         (aes_256_args_t){.iv = iv,
                                                          .key = enc_key,
                                                          .in = ciphertext,
                                                          .out = plaintext,
                                                          .bytes_written = bytes_written,
                                                          .status = status})) {
            return false;
        }
        BSON_ASSERT(*bytes_written == plaintext->len);
    }

    return true;
}

/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_do_decryption --
 *
 *    Defer decryption to whichever crypto library libmongocrypt is using.
 *
 * Parameters:
 *    @associated_data associated data for the HMAC. May be NULL.
 *    @key a 96 byte key.
 *    @ciphertext the ciphertext to decrypt. This contains the IV prepended.
 *    @plaintext a location for the resulting plaintext.
 *    @bytes_written a location for the resulting bytes written.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 *  Preconditions:
 *    1. plaintext->data has been pre-allocated with enough space for the
 *    resulting plaintext and padding. See _mongocrypt_calculate_plaintext_len.
 *
 *  Postconditions:
 *    1. bytes_written is set to the length of the written plaintext, excluding
 *    padding. This may be less than
 *    _mongocrypt_calculate_plaintext_len (ciphertext->len, mode, hmac, status).
 *
 * ----------------------------------------------------------------------------
 */
static bool _mongocrypt_do_decryption(_mongocrypt_crypto_t *crypto,
                                      _mongocrypt_key_format_t key_format,
                                      _mongocrypt_mac_format_t mac_format,
                                      _mongocrypt_encryption_mode_t mode,
                                      _mongocrypt_hmac_type_t hmac,
                                      const _mongocrypt_buffer_t *associated_data,
                                      const _mongocrypt_buffer_t *key,
                                      const _mongocrypt_buffer_t *ciphertext,
                                      _mongocrypt_buffer_t *plaintext,
                                      uint32_t *bytes_written,
                                      mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    /* associated_data is checked at the point it is used, so it can be NULL */
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT_PARAM(plaintext);
    BSON_ASSERT_PARAM(bytes_written);

    const uint32_t expect_plaintext_len = _mongocrypt_calculate_plaintext_len(ciphertext->len, mode, hmac, status);
    if (mongocrypt_status_type(status) != MONGOCRYPT_STATUS_OK) {
        return false;
    }
    if (plaintext->len != expect_plaintext_len) {
        CLIENT_ERR("output plaintext should have been allocated with %d bytes, "
                   "but has: %d",
                   expect_plaintext_len,
                   plaintext->len);
        return false;
    }
    if (expect_plaintext_len == 0) {
        // While a ciphertext string describing a zero length plaintext is
        // technically valid,
        // it's not actually particularly useful in the context of FLE where such
        // values aren't encoded.
        CLIENT_ERR("input ciphertext too small. Must be more than %" PRIu32 " bytes",
                   _mongocrypt_calculate_ciphertext_len(0, mode, hmac, NULL));
        return false;
    }

    const uint32_t expected_key_len = (key_format == KEY_FORMAT_FLE2) ? MONGOCRYPT_ENC_KEY_LEN : MONGOCRYPT_KEY_LEN;
    if (expected_key_len != key->len) {
        CLIENT_ERR("key should have length %d, but has length %d", expected_key_len, key->len);
        return false;
    }

    const uint32_t min_cipherlen = _mongocrypt_calculate_ciphertext_len(0, mode, hmac, NULL);
    if (ciphertext->len < min_cipherlen) {
        CLIENT_ERR("corrupt ciphertext - must be >= %d bytes", min_cipherlen);
        return false;
    }

    _mongocrypt_buffer_t Ke;
    const uint32_t Ke_offset = (key_format == KEY_FORMAT_FLE1) ? MONGOCRYPT_MAC_KEY_LEN : 0;
    if (!_mongocrypt_buffer_from_subrange(&Ke, key, Ke_offset, MONGOCRYPT_ENC_KEY_LEN)) {
        CLIENT_ERR("unable to create Ke subrange from key");
        return false;
    }

    _mongocrypt_buffer_t IV;
    if (!_mongocrypt_buffer_from_subrange(&IV, ciphertext, 0, MONGOCRYPT_IV_LEN)) {
        CLIENT_ERR("unable to create IV subrange from ciphertext");
        return false;
    }

    if (hmac == HMAC_NONE) {
        BSON_ASSERT(key_format == KEY_FORMAT_FLE2);

    } else {
        BSON_ASSERT(key_format != KEY_FORMAT_FLE2);

        uint8_t hmac_tag_storage[MONGOCRYPT_HMAC_LEN];
        const uint32_t mac_key_offset = (key_format == KEY_FORMAT_FLE1) ? 0 : MONGOCRYPT_ENC_KEY_LEN;
        _mongocrypt_buffer_t Km;
        if (!_mongocrypt_buffer_from_subrange(&Km, key, mac_key_offset, MONGOCRYPT_MAC_KEY_LEN)) {
            CLIENT_ERR("unable to create Km subrange from key");
            return false;
        }

        _mongocrypt_buffer_t iv_and_ciphertext;
        if (!_mongocrypt_buffer_from_subrange(&iv_and_ciphertext,
                                              ciphertext,
                                              0,
                                              ciphertext->len - MONGOCRYPT_HMAC_LEN)) {
            CLIENT_ERR("unable to create IV || S subrange from C");
            return false;
        }

        _mongocrypt_buffer_t hmac_tag = {.data = hmac_tag_storage, .len = MONGOCRYPT_HMAC_LEN};

        if (!_hmac_step(crypto, mac_format, hmac, &Km, associated_data, &iv_and_ciphertext, &hmac_tag, status)) {
            return false;
        }

        /* Constant time compare. */
        _mongocrypt_buffer_t T;
        if (!_mongocrypt_buffer_from_subrange(&T,
                                              ciphertext,
                                              ciphertext->len - MONGOCRYPT_HMAC_LEN,
                                              MONGOCRYPT_HMAC_LEN)) {
            CLIENT_ERR("unable to create T subrange from C");
            return false;
        }
        if (0 != _mongocrypt_memequal(hmac_tag.data, T.data, MONGOCRYPT_HMAC_LEN)) {
            CLIENT_ERR("HMAC validation failure");
            return false;
        }
    }

    /* Decrypt data excluding IV + HMAC. */
    const uint32_t hmac_len = (hmac == HMAC_NONE) ? 0 : MONGOCRYPT_HMAC_LEN;
    _mongocrypt_buffer_t S;
    if (!_mongocrypt_buffer_from_subrange(&S,
                                          ciphertext,
                                          MONGOCRYPT_IV_LEN,
                                          ciphertext->len - MONGOCRYPT_IV_LEN - hmac_len)) {
        CLIENT_ERR("unable to create S subrange from C");
        return false;
    }

    return _decrypt_step(crypto, mode, &IV, &Ke, &S, plaintext, bytes_written, status);
}

#define DECLARE_ALGORITHM(name, mode, hmac)                                                                            \
    static uint32_t _mc_##name##_ciphertext_len(uint32_t plaintext_len, mongocrypt_status_t *status) {                 \
        return _mongocrypt_calculate_ciphertext_len(plaintext_len, MODE_##mode, HMAC_##hmac, status);                  \
    }                                                                                                                  \
    static uint32_t _mc_##name##_plaintext_len(uint32_t ciphertext_len, mongocrypt_status_t *status) {                 \
        return _mongocrypt_calculate_plaintext_len(ciphertext_len, MODE_##mode, HMAC_##hmac, status);                  \
    }                                                                                                                  \
    static bool _mc_##name##_do_encryption(_mongocrypt_crypto_t *crypto,                                               \
                                           const _mongocrypt_buffer_t *iv,                                             \
                                           const _mongocrypt_buffer_t *aad,                                            \
                                           const _mongocrypt_buffer_t *key,                                            \
                                           const _mongocrypt_buffer_t *plaintext,                                      \
                                           _mongocrypt_buffer_t *ciphertext,                                           \
                                           uint32_t *written,                                                          \
                                           mongocrypt_status_t *status) {                                              \
        return _mongocrypt_do_encryption(crypto,                                                                       \
                                         KEY_FORMAT_##name,                                                            \
                                         MAC_FORMAT_##name,                                                            \
                                         MODE_##mode,                                                                  \
                                         HMAC_##hmac,                                                                  \
                                         iv,                                                                           \
                                         aad,                                                                          \
                                         key,                                                                          \
                                         plaintext,                                                                    \
                                         ciphertext,                                                                   \
                                         written,                                                                      \
                                         status);                                                                      \
    }                                                                                                                  \
    static bool _mc_##name##_do_decryption(_mongocrypt_crypto_t *crypto,                                               \
                                           const _mongocrypt_buffer_t *aad,                                            \
                                           const _mongocrypt_buffer_t *key,                                            \
                                           const _mongocrypt_buffer_t *ciphertext,                                     \
                                           _mongocrypt_buffer_t *plaintext,                                            \
                                           uint32_t *written,                                                          \
                                           mongocrypt_status_t *status) {                                              \
        return _mongocrypt_do_decryption(crypto,                                                                       \
                                         KEY_FORMAT_##name,                                                            \
                                         MAC_FORMAT_##name,                                                            \
                                         MODE_##mode,                                                                  \
                                         HMAC_##hmac,                                                                  \
                                         aad,                                                                          \
                                         key,                                                                          \
                                         ciphertext,                                                                   \
                                         plaintext,                                                                    \
                                         written,                                                                      \
                                         status);                                                                      \
    }                                                                                                                  \
    static const _mongocrypt_value_encryption_algorithm_t _mc##name##Algorithm_definition = {                          \
        _mc_##name##_ciphertext_len,                                                                                   \
        _mc_##name##_plaintext_len,                                                                                    \
        _mc_##name##_do_encryption,                                                                                    \
        _mc_##name##_do_decryption,                                                                                    \
    };                                                                                                                 \
    const _mongocrypt_value_encryption_algorithm_t *_mc##name##Algorithm() { return &_mc##name##Algorithm_definition; }

// FLE1 algorithm: AES-256-CBC HMAC/SHA-512-256 (SHA-512 truncated to 256 bits)
DECLARE_ALGORITHM(FLE1, CBC, SHA_512_256)

// FLE2 AEAD used value algorithm: AES-256-CTR HMAC/SHA-256
DECLARE_ALGORITHM(FLE2AEAD, CTR, SHA_256)

// FLE2 used with ESC/ECOC tokens: AES-256-CTR no HMAC
DECLARE_ALGORITHM(FLE2, CTR, NONE)

// FLE2v2 AEAD general algorithm: AES-256-CBC HMAC/SHA-256
DECLARE_ALGORITHM(FLE2v2AEAD, CBC, SHA_256)

#undef DECLARE_ALGORITHM

/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_random --
 *
 *    Generates a string of random bytes.
 *
 * Parameters:
 *    @out an output buffer that has been pre-allocated.
 *    @status set on error.
 *    @count the size of the random string in bytes.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 *  Preconditions:
 *    1. out has been pre-allocated with at least 'count' bytes of space.
 *
 * ----------------------------------------------------------------------------
 */
bool _mongocrypt_random(_mongocrypt_crypto_t *crypto,
                        _mongocrypt_buffer_t *out,
                        uint32_t count,
                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(out);

    if (count != out->len) {
        CLIENT_ERR("out should have length %d, but has length %d", count, out->len);
        return false;
    }

    return _crypto_random(crypto, out, count, status);
}

/* ----------------------------------------------------------------------------
 *
 * _mongocrypt_calculate_deterministic_iv --
 *
 *    Compute the IV for deterministic encryption from the plaintext and IV
 *    key by using HMAC function.
 *
 * Parameters:
 *    @key the 96 byte key. The last 32 represent the IV key.
 *    @plaintext the plaintext to be encrypted.
 *    @associated_data associated data to include in the HMAC.
 *    @out an output buffer that has been pre-allocated.
 *    @status set on error.
 *
 * Returns:
 *    True on success. On error, sets @status and returns false.
 *
 *  Preconditions:
 *    1. out has been pre-allocated with at least MONGOCRYPT_IV_LEN bytes.
 *
 * ----------------------------------------------------------------------------
 */
bool _mongocrypt_calculate_deterministic_iv(_mongocrypt_crypto_t *crypto,
                                            const _mongocrypt_buffer_t *key,
                                            const _mongocrypt_buffer_t *plaintext,
                                            const _mongocrypt_buffer_t *associated_data,
                                            _mongocrypt_buffer_t *out,
                                            mongocrypt_status_t *status) {
    _mongocrypt_buffer_t intermediates[3];
    _mongocrypt_buffer_t to_hmac;
    _mongocrypt_buffer_t iv_key;
    uint64_t associated_data_len_be;
    uint8_t tag_storage[64];
    _mongocrypt_buffer_t tag;
    bool ret = false;

    _mongocrypt_buffer_init(&to_hmac);

    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(plaintext);
    BSON_ASSERT_PARAM(associated_data);
    BSON_ASSERT_PARAM(out);

    if (MONGOCRYPT_KEY_LEN != key->len) {
        CLIENT_ERR("key should have length %d, but has length %d\n", MONGOCRYPT_KEY_LEN, key->len);
        goto done;
    }
    if (MONGOCRYPT_IV_LEN != out->len) {
        CLIENT_ERR("out should have length %d, but has length %d\n", MONGOCRYPT_IV_LEN, out->len);
        goto done;
    }

    _mongocrypt_buffer_init(&iv_key);
    iv_key.data = key->data + MONGOCRYPT_ENC_KEY_LEN + MONGOCRYPT_MAC_KEY_LEN;
    iv_key.len = MONGOCRYPT_IV_KEY_LEN;

    _mongocrypt_buffer_init(&intermediates[0]);
    _mongocrypt_buffer_init(&intermediates[1]);
    _mongocrypt_buffer_init(&intermediates[2]);
    /* Add associated data. */
    intermediates[0].data = associated_data->data;
    intermediates[0].len = associated_data->len;
    /* Add associated data length in bits. */
    /* multiplying a uint32_t by 8 won't bring it anywhere close to UINT64_MAX */
    associated_data_len_be = 8 * (uint64_t)associated_data->len;
    associated_data_len_be = BSON_UINT64_TO_BE(associated_data_len_be);
    intermediates[1].data = (uint8_t *)&associated_data_len_be;
    intermediates[1].len = sizeof(uint64_t);
    /* Add plaintext. */
    intermediates[2].data = (uint8_t *)plaintext->data;
    intermediates[2].len = plaintext->len;

    tag.data = tag_storage;
    tag.len = sizeof(tag_storage);

    if (!_mongocrypt_buffer_concat(&to_hmac, intermediates, 3)) {
        CLIENT_ERR("failed to allocate buffer");
        goto done;
    }

    if (!_crypto_hmac_sha_512(crypto, &iv_key, &to_hmac, &tag, status)) {
        goto done;
    }

    /* Truncate to IV length */
    memcpy(out->data, tag.data, MONGOCRYPT_IV_LEN);

    ret = true;
done:
    _mongocrypt_buffer_cleanup(&to_hmac);
    return ret;
}

bool _mongocrypt_wrap_key(_mongocrypt_crypto_t *crypto,
                          _mongocrypt_buffer_t *kek,
                          _mongocrypt_buffer_t *dek,
                          _mongocrypt_buffer_t *encrypted_dek,
                          mongocrypt_status_t *status) {
    const _mongocrypt_value_encryption_algorithm_t *fle1alg = _mcFLE1Algorithm();
    uint32_t bytes_written;
    _mongocrypt_buffer_t iv = {0};
    bool ret = false;

    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(kek);
    BSON_ASSERT_PARAM(dek);
    BSON_ASSERT_PARAM(encrypted_dek);

    _mongocrypt_buffer_init(encrypted_dek);

    if (dek->len != MONGOCRYPT_KEY_LEN) {
        CLIENT_ERR("data encryption key is incorrect length, expected: %" PRIu32 ", got: %" PRIu32,
                   MONGOCRYPT_KEY_LEN,
                   dek->len);
        goto done;
    }

    // _mongocrypt_wrap_key() uses FLE1 algorithm parameters.
    _mongocrypt_buffer_resize(encrypted_dek, fle1alg->get_ciphertext_len(dek->len, status));
    _mongocrypt_buffer_resize(&iv, MONGOCRYPT_IV_LEN);

    if (!_mongocrypt_random(crypto, &iv, MONGOCRYPT_IV_LEN, status)) {
        goto done;
    }

    if (!fle1alg
             ->do_encrypt(crypto, &iv, NULL /* associated data. */, kek, dek, encrypted_dek, &bytes_written, status)) {
        goto done;
    }

    ret = true;
done:
    _mongocrypt_buffer_cleanup(&iv);
    return ret;
}

bool _mongocrypt_unwrap_key(_mongocrypt_crypto_t *crypto,
                            _mongocrypt_buffer_t *kek,
                            _mongocrypt_buffer_t *encrypted_dek,
                            _mongocrypt_buffer_t *dek,
                            mongocrypt_status_t *status) {
    const _mongocrypt_value_encryption_algorithm_t *fle1alg = _mcFLE1Algorithm();
    uint32_t bytes_written;

    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(kek);
    BSON_ASSERT_PARAM(dek);
    BSON_ASSERT_PARAM(encrypted_dek);

    // _mongocrypt_wrap_key() uses FLE1 algorithm parameters.
    _mongocrypt_buffer_init(dek);
    _mongocrypt_buffer_resize(dek, fle1alg->get_plaintext_len(encrypted_dek->len, status));

    if (!fle1alg->do_decrypt(crypto, NULL /* associated data. */, kek, encrypted_dek, dek, &bytes_written, status)) {
        return false;
    }
    dek->len = bytes_written;

    if (dek->len != MONGOCRYPT_KEY_LEN) {
        CLIENT_ERR("decrypted key is incorrect length, expected: %" PRIu32 ", got: %" PRIu32,
                   MONGOCRYPT_KEY_LEN,
                   dek->len);
        return false;
    }
    return true;
}

/* This implementation avoids modulo bias. It is based on arc4random_uniform:
https://github.com/openbsd/src/blob/2207c4325726fdc5c4bcd0011af0fdf7d3dab137/lib/libc/crypt/arc4random_uniform.c#L33
*/
bool _mongocrypt_random_uint64(_mongocrypt_crypto_t *crypto,
                               uint64_t exclusive_upper_bound,
                               uint64_t *out,
                               mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(out);

    *out = 0;

    if (exclusive_upper_bound < 2) {
        *out = 0;
        return true;
    }

    /* 2**64 % x == (2**64 - x) % x */
    uint64_t min = (0 - exclusive_upper_bound) % exclusive_upper_bound;

    _mongocrypt_buffer_t rand_u64_buf;
    _mongocrypt_buffer_init(&rand_u64_buf);
    _mongocrypt_buffer_resize(&rand_u64_buf, (uint32_t)sizeof(uint64_t));

    uint64_t rand_u64;
    for (;;) {
        if (!_mongocrypt_random(crypto, &rand_u64_buf, rand_u64_buf.len, status)) {
            _mongocrypt_buffer_cleanup(&rand_u64_buf);
            return false;
        }

        memcpy(&rand_u64, rand_u64_buf.data, rand_u64_buf.len);

        if (rand_u64 >= min) {
            break;
        }
    }

    *out = rand_u64 % exclusive_upper_bound;

    _mongocrypt_buffer_cleanup(&rand_u64_buf);
    return true;
}

bool _mongocrypt_random_int64(_mongocrypt_crypto_t *crypto,
                              int64_t exclusive_upper_bound,
                              int64_t *out,
                              mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(out);

    if (exclusive_upper_bound <= 0) {
        CLIENT_ERR("Expected exclusive_upper_bound > 0");
        return false;
    }

    uint64_t u64_exclusive_upper_bound = (uint64_t)exclusive_upper_bound;
    uint64_t u64_out;

    if (!_mongocrypt_random_uint64(crypto, u64_exclusive_upper_bound, &u64_out, status)) {
        return false;
    }

    /* Zero the leading bit to ensure rand_i64 is non-negative. */
    u64_out &= (~(1ull << 63));
    *out = (int64_t)u64_out;
    return true;
}
