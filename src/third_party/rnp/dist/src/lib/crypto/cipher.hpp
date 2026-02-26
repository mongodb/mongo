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
#ifndef RNP_CIPHER_HPP
#define RNP_CIPHER_HPP

#include <memory>
#include <string>
#include <repgp/repgp_def.h>

// Note: for AEAD modes we append the authentication tag to the ciphertext as in RFC 5116
class Cipher {
  public:
    // the tag size should be 0 for non-AEAD and must be non-zero for AEAD modes (no default)
    static std::unique_ptr<Cipher> encryption(pgp_symm_alg_t    cipher,
                                              pgp_cipher_mode_t mode,
                                              size_t            tag_size = 0,
                                              bool              disable_padding = false);
    static std::unique_ptr<Cipher> decryption(pgp_symm_alg_t    cipher,
                                              pgp_cipher_mode_t mode,
                                              size_t            tag_size = 0,
                                              bool              disable_padding = false);

    virtual bool set_key(const uint8_t *key, size_t key_length) = 0;
    virtual bool set_iv(const uint8_t *iv, size_t iv_length) = 0;
    // only valid for AEAD modes
    virtual bool set_ad(const uint8_t *ad, size_t ad_length) = 0;

    virtual size_t block_size() const;
    virtual size_t update_granularity() const = 0;

    // input_length must be a multiple of update_granularity
    virtual bool update(uint8_t *      output,
                        size_t         output_length,
                        size_t *       output_written,
                        const uint8_t *input,
                        size_t         input_length,
                        size_t *       input_consumed) = 0;
    /**
     * @brief Finalize cipher. For AEAD mode, depending on backend, may require whole
     * authentication tag to be present in input.
     */
    virtual bool finish(uint8_t *      output,
                        size_t         output_length,
                        size_t *       output_written,
                        const uint8_t *input,
                        size_t         input_length,
                        size_t *       input_consumed) = 0;

    virtual ~Cipher();

  protected:
    Cipher(pgp_symm_alg_t alg);

    pgp_symm_alg_t m_alg;
};

#endif
