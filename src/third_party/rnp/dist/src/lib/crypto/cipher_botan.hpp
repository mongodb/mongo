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
#ifndef RNP_CIPHER_BOTAN_HPP
#define RNP_CIPHER_BOTAN_HPP

#include "cipher.hpp"
#include <botan/cipher_mode.h>
#include <repgp/repgp_def.h>

class Cipher_Botan : public Cipher {
  public:
    static std::unique_ptr<Cipher_Botan> encryption(pgp_symm_alg_t    cipher,
                                                    pgp_cipher_mode_t mode,
                                                    size_t            tag_size,
                                                    bool              disable_padding);
    static std::unique_ptr<Cipher_Botan> decryption(pgp_symm_alg_t    cipher,
                                                    pgp_cipher_mode_t mode,
                                                    size_t            tag_size,
                                                    bool              disable_padding);

    bool set_key(const uint8_t *key, size_t key_length) override;
    bool set_iv(const uint8_t *iv, size_t iv_length) override;
    bool set_ad(const uint8_t *ad, size_t ad_length) override;

    size_t update_granularity() const override;

    bool update(uint8_t *      output,
                size_t         output_length,
                size_t *       output_written,
                const uint8_t *input,
                size_t         input_length,
                size_t *       input_consumed) override;
    bool finish(uint8_t *      output,
                size_t         output_length,
                size_t *       output_written,
                const uint8_t *input,
                size_t         input_length,
                size_t *       input_consumed) override;
    virtual ~Cipher_Botan();

  private:
    Cipher_Botan(pgp_symm_alg_t alg, std::unique_ptr<Botan::Cipher_Mode> cipher);

    std::unique_ptr<Botan::Cipher_Mode> m_cipher;
    std::vector<uint8_t>                m_buf;

    static Cipher_Botan *create(pgp_symm_alg_t alg, const std::string &name, bool encrypt);
};

#endif
