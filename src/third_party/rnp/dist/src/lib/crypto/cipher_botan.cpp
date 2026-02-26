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
#include <sstream>
#include <cassert>
#include <botan/aead.h>
#include "cipher_botan.hpp"
#include "utils.h"
#include "types.h"

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

Cipher_Botan *
Cipher_Botan::create(pgp_symm_alg_t alg, const std::string &name, bool encrypt)
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
#if defined(CRYPTO_BACKEND_BOTAN3)
    auto dir = encrypt ? Botan::Cipher_Dir::Encryption : Botan::Cipher_Dir::Decryption;
#else
    auto dir = encrypt ? Botan::Cipher_Dir::ENCRYPTION : Botan::Cipher_Dir::DECRYPTION;
#endif
    auto cipher = Botan::Cipher_Mode::create(name, dir);
    if (!cipher) {
        RNP_LOG("Failed to create cipher '%s'", name.c_str());
        return nullptr;
    }
    return new (std::nothrow) Cipher_Botan(alg, std::move(cipher));
}

static std::string
make_name(pgp_symm_alg_t cipher, pgp_cipher_mode_t mode, size_t tag_size, bool disable_padding)
{
    const char *cipher_string = id_str_pair::lookup(cipher_map, cipher, NULL);
    const char *mode_string = id_str_pair::lookup(cipher_mode_map, mode, NULL);
    if (!cipher_string || !mode_string) {
        return "";
    }
    try {
        std::stringstream ss;
        ss << cipher_string << "/" << mode_string;
        if (tag_size) {
            ss << "(" << tag_size << ")";
        }
        if (mode == PGP_CIPHER_MODE_CBC && disable_padding) {
            ss << "/NoPadding";
        }
        return ss.str();
    } catch (const std::exception &e) {
        return "";
    }
}

std::unique_ptr<Cipher_Botan>
Cipher_Botan::encryption(pgp_symm_alg_t    cipher,
                         pgp_cipher_mode_t mode,
                         size_t            tag_size,
                         bool              disable_padding)
{
    return std::unique_ptr<Cipher_Botan>(
      create(cipher, make_name(cipher, mode, tag_size, disable_padding), true));
}

std::unique_ptr<Cipher_Botan>
Cipher_Botan::decryption(pgp_symm_alg_t    cipher,
                         pgp_cipher_mode_t mode,
                         size_t            tag_size,
                         bool              disable_padding)
{
    return std::unique_ptr<Cipher_Botan>(
      create(cipher, make_name(cipher, mode, tag_size, disable_padding), false));
}

size_t
Cipher_Botan::update_granularity() const
{
    return m_cipher->update_granularity();
}

bool
Cipher_Botan::set_key(const uint8_t *key, size_t key_length)
{
    try {
        m_cipher->set_key(key, key_length);
    } catch (const std::exception &e) {
        RNP_LOG("Failed to set key: %s", e.what());
        return false;
    }
    return true;
}

bool
Cipher_Botan::set_iv(const uint8_t *iv, size_t iv_length)
{
    try {
        m_cipher->start(iv, iv_length);
        m_buf.reserve(this->update_granularity());
    } catch (const std::exception &e) {
        RNP_LOG("Failed to set IV: %s", e.what());
        return false;
    }
    return true;
}

bool
Cipher_Botan::set_ad(const uint8_t *ad, size_t ad_length)
{
    assert(m_cipher->authenticated());
    try {
        dynamic_cast<Botan::AEAD_Mode &>(*m_cipher).set_associated_data(ad, ad_length);
    } catch (const std::exception &e) {
        RNP_LOG("Failed to set AAD: %s", e.what());
        return false;
    }
    return true;
}

bool
Cipher_Botan::update(uint8_t *      output,
                     size_t         output_length,
                     size_t *       output_written,
                     const uint8_t *input,
                     size_t         input_length,
                     size_t *       input_consumed)
{
    try {
        size_t ud = this->update_granularity();
        m_buf.resize(ud);

        *input_consumed = 0;
        *output_written = 0;
        while (input_length >= ud && output_length >= ud) {
            m_buf.assign(input, input + ud);
            size_t written = m_cipher->process(m_buf.data(), ud);
            std::copy(m_buf.data(), m_buf.data() + written, output);
            input += ud;
            output += written;
            input_length -= ud;
            output_length -= written;

            *output_written += written;
            *input_consumed += ud;
        }
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return false;
    }
    return true;
}

bool
Cipher_Botan::finish(uint8_t *      output,
                     size_t         output_length,
                     size_t *       output_written,
                     const uint8_t *input,
                     size_t         input_length,
                     size_t *       input_consumed)
{
    try {
        *input_consumed = 0;
        *output_written = 0;
        size_t ud = this->update_granularity();
        if (input_length > ud) {
            if (!update(output,
                        output_length,
                        output_written,
                        input,
                        input_length - ud,
                        input_consumed)) {
                return false;
            }
            input += *input_consumed;
            input_length = input_length - *input_consumed;
            output += *output_written;
            output_length -= *output_written;
        }
        Botan::secure_vector<uint8_t> final_block(input, input + input_length);
        m_cipher->finish(final_block);
        if (final_block.size() > output_length) {
            RNP_LOG("Insufficient buffer");
            return false;
        }
        std::copy(final_block.begin(), final_block.end(), output);
        *output_written += final_block.size();
        *input_consumed += input_length;
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return false;
    }
    return true;
}

Cipher_Botan::Cipher_Botan(pgp_symm_alg_t alg, std::unique_ptr<Botan::Cipher_Mode> cipher)
    : Cipher(alg), m_cipher(std::move(cipher))
{
}

Cipher_Botan::~Cipher_Botan()
{
}
