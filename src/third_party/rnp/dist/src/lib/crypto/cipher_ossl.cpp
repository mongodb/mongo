/*
 * Copyright (c) 2021, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <cassert>
#include <algorithm>

#include "cipher_ossl.hpp"
#include "utils.h"
#include "types.h"
#include <openssl/err.h>

static const id_str_pair cipher_mode_map[] = {
  {PGP_CIPHER_MODE_CBC, "CBC"},
  {PGP_CIPHER_MODE_OCB, "OCB"},
  {0, NULL},
};

static const id_str_pair cipher_map[] = {
  {PGP_SA_AES_128, "AES-128"},
  {PGP_SA_AES_256, "AES-256"},
  {PGP_SA_IDEA, "IDEA"},
  {0, NULL},
};

EVP_CIPHER_CTX *
Cipher_OpenSSL::create(pgp_symm_alg_t     alg,
                       const std::string &name,
                       bool               encrypt,
                       size_t             tag_size,
                       bool               disable_padding)
{
#if !defined(ENABLE_IDEA)
    if (alg == PGP_SA_IDEA) {
        RNP_LOG("IDEA support has been disabled");
        return nullptr;
    }
#endif
#if !defined(ENABLE_BLOWFISH)
    if (alg == PGP_SA_BLOWFISH) {
        RNP_LOG("Blowfish support has been disabled");
        return nullptr;
    }
#endif
#if !defined(ENABLE_CAST5)
    if (alg == PGP_SA_CAST5) {
        RNP_LOG("CAST5 support has been disabled");
        return nullptr;
    }
#endif
    const EVP_CIPHER *cipher = EVP_get_cipherbyname(name.c_str());
    if (!cipher) {
        RNP_LOG("Unsupported cipher: %s", name.c_str());
        return nullptr;
    }
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        RNP_LOG("Failed to create cipher context: %lu", ERR_peek_last_error());
        return nullptr;
    }
    if (EVP_CipherInit_ex(ctx, cipher, NULL, NULL, NULL, encrypt ? 1 : 0) != 1) {
        RNP_LOG("Failed to initialize cipher: %lu", ERR_peek_last_error());
        EVP_CIPHER_CTX_free(ctx);
        return nullptr;
    }
    // set tag size
    if (encrypt && tag_size) {
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, tag_size, NULL) != 1) {
            RNP_LOG("Failed to set AEAD tag length: %lu", ERR_peek_last_error());
            EVP_CIPHER_CTX_free(ctx);
            return nullptr;
        }
    }
    if (disable_padding) {
        EVP_CIPHER_CTX_set_padding(ctx, 0);
    }
    return ctx;
}

static std::string
make_name(pgp_symm_alg_t cipher, pgp_cipher_mode_t mode)
{
    const char *cipher_string = id_str_pair::lookup(cipher_map, cipher, NULL);
    const char *mode_string = id_str_pair::lookup(cipher_mode_map, mode, NULL);
    if (!cipher_string || !mode_string) {
        return "";
    }
    return std::string(cipher_string) + "-" + mode_string;
}

std::unique_ptr<Cipher_OpenSSL>
Cipher_OpenSSL::encryption(pgp_symm_alg_t    cipher,
                           pgp_cipher_mode_t mode,
                           size_t            tag_size,
                           bool              disable_padding)
{
    EVP_CIPHER_CTX *ossl_ctx =
      create(cipher, make_name(cipher, mode), true, tag_size, disable_padding);
    if (!ossl_ctx) {
        return NULL;
    }
    return std::unique_ptr<Cipher_OpenSSL>(new (std::nothrow)
                                             Cipher_OpenSSL(cipher, ossl_ctx, tag_size, true));
}

std::unique_ptr<Cipher_OpenSSL>
Cipher_OpenSSL::decryption(pgp_symm_alg_t    cipher,
                           pgp_cipher_mode_t mode,
                           size_t            tag_size,
                           bool              disable_padding)
{
    EVP_CIPHER_CTX *ossl_ctx =
      create(cipher, make_name(cipher, mode), false, tag_size, disable_padding);
    if (!ossl_ctx) {
        return NULL;
    }
    return std::unique_ptr<Cipher_OpenSSL>(
      new (std::nothrow) Cipher_OpenSSL(cipher, ossl_ctx, tag_size, false));
}

bool
Cipher_OpenSSL::set_key(const uint8_t *key, size_t key_length)
{
    assert(key_length <= INT_MAX);
    return EVP_CIPHER_CTX_set_key_length(m_ctx, (int) key_length) == 1 &&
           EVP_CipherInit_ex(m_ctx, NULL, NULL, key, NULL, -1) == 1;
}

bool
Cipher_OpenSSL::set_iv(const uint8_t *iv, size_t iv_length)
{
    assert(iv_length <= INT_MAX);
    // set IV len for AEAD modes
    if (m_tag_size &&
        EVP_CIPHER_CTX_ctrl(m_ctx, EVP_CTRL_AEAD_SET_IVLEN, (int) iv_length, NULL) != 1) {
        RNP_LOG("Failed to set AEAD IV length: %lu", ERR_peek_last_error());
        return false;
    }
    if (EVP_CIPHER_CTX_iv_length(m_ctx) != (int) iv_length) {
        RNP_LOG("IV length mismatch");
        return false;
    }
    if (EVP_CipherInit_ex(m_ctx, NULL, NULL, NULL, iv, -1) != 1) {
        RNP_LOG("Failed to set IV: %lu", ERR_peek_last_error());
    }
    return true;
}

bool
Cipher_OpenSSL::set_ad(const uint8_t *ad, size_t ad_length)
{
    assert(m_tag_size);
    int outlen = 0;
    if (EVP_CipherUpdate(m_ctx, NULL, &outlen, ad, ad_length) != 1) {
        RNP_LOG("Failed to set AD: %lu", ERR_peek_last_error());
        return false;
    }
    return true;
}

size_t
Cipher_OpenSSL::update_granularity() const
{
    return (size_t) EVP_CIPHER_CTX_block_size(m_ctx);
}

bool
Cipher_OpenSSL::update(uint8_t *      output,
                       size_t         output_length,
                       size_t *       output_written,
                       const uint8_t *input,
                       size_t         input_length,
                       size_t *       input_consumed)
{
    if (input_length > INT_MAX) {
        return false;
    }
    *input_consumed = 0;
    *output_written = 0;
    if (input_length == 0) {
        return true;
    }
    int outl = 0;
    if (EVP_CipherUpdate(m_ctx, output, &outl, input, (int) input_length) != 1) {
        RNP_LOG("EVP_CipherUpdate failed: %lu", ERR_peek_last_error());
        return false;
    }
    assert((size_t) outl < output_length);
    *input_consumed = input_length;
    *output_written = (size_t) outl;
    return true;
}

bool
Cipher_OpenSSL::finish(uint8_t *      output,
                       size_t         output_length,
                       size_t *       output_written,
                       const uint8_t *input,
                       size_t         input_length,
                       size_t *       input_consumed)
{
    if (input_length > INT_MAX) {
        return false;
    }
    if (!m_encrypt && input_length < m_tag_size) {
        RNP_LOG("Insufficient input for final block (missing tag)");
        return false;
    }
    *input_consumed = 0;
    *output_written = 0;
    if (!m_encrypt && m_tag_size) {
        // set the tag from the end of the ciphertext
        if (EVP_CIPHER_CTX_ctrl(m_ctx,
                                EVP_CTRL_AEAD_SET_TAG,
                                m_tag_size,
                                const_cast<uint8_t *>(input) + input_length - m_tag_size) !=
            1) {
            RNP_LOG("Failed to set expected AEAD tag: %lu", ERR_peek_last_error());
            return false;
        }
        size_t ats = std::min(m_tag_size, input_length);
        input_length -= ats;    // m_tag_size;
        *input_consumed += ats; // m_tag_size;
    }
    int outl = 0;
    if (EVP_CipherUpdate(m_ctx, output, &outl, input, (int) input_length) != 1) {
        RNP_LOG("EVP_CipherUpdate failed: %lu", ERR_peek_last_error());
        return false;
    }
    input += input_length;
    *input_consumed += input_length;
    output += outl;
    *output_written += (size_t) outl;
    if (EVP_CipherFinal_ex(m_ctx, output, &outl) != 1) {
        RNP_LOG("EVP_CipherFinal_ex failed: %lu", ERR_peek_last_error());
        return false;
    }
    *output_written += (size_t) outl;
    output += (size_t) outl;
    if (m_encrypt && m_tag_size) {
        // append the tag
        if (EVP_CIPHER_CTX_ctrl(m_ctx, EVP_CTRL_AEAD_GET_TAG, m_tag_size, output) != 1) {
            RNP_LOG("Failed to append AEAD tag: %lu", ERR_peek_last_error());
            return false;
        }
        *output_written += m_tag_size;
    }
    return true;
}

Cipher_OpenSSL::Cipher_OpenSSL(pgp_symm_alg_t  alg,
                               EVP_CIPHER_CTX *ctx,
                               size_t          tag_size,
                               bool            encrypt)
    : Cipher(alg), m_ctx(ctx), m_tag_size(tag_size), m_encrypt(encrypt)
{
    m_block_size = EVP_CIPHER_CTX_block_size(m_ctx);
}

Cipher_OpenSSL::~Cipher_OpenSSL()
{
    EVP_CIPHER_CTX_free(m_ctx);
}
