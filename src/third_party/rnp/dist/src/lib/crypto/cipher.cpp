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
#include "config.h"
#include "symmetric.h"
#include "cipher.hpp"

#if defined(CRYPTO_BACKEND_OPENSSL)
#include "cipher_ossl.hpp"
#elif defined(CRYPTO_BACKEND_BOTAN)
#include "cipher_botan.hpp"
#endif

std::unique_ptr<Cipher>
Cipher::encryption(pgp_symm_alg_t    cipher,
                   pgp_cipher_mode_t mode,
                   size_t            tag_size,
                   bool              disable_padding)
{
#if defined(CRYPTO_BACKEND_OPENSSL)
    return Cipher_OpenSSL::encryption(cipher, mode, tag_size, disable_padding);
#elif defined(CRYPTO_BACKEND_BOTAN)
    return Cipher_Botan::encryption(cipher, mode, tag_size, disable_padding);
#else
#error "Crypto backend not specified"
#endif
}

std::unique_ptr<Cipher>
Cipher::decryption(pgp_symm_alg_t    cipher,
                   pgp_cipher_mode_t mode,
                   size_t            tag_size,
                   bool              disable_padding)
{
#if defined(CRYPTO_BACKEND_OPENSSL)
    return Cipher_OpenSSL::decryption(cipher, mode, tag_size, disable_padding);
#elif defined(CRYPTO_BACKEND_BOTAN)
    return Cipher_Botan::decryption(cipher, mode, tag_size, disable_padding);
#else
#error "Crypto backend not specified"
#endif
}

Cipher::Cipher(pgp_symm_alg_t alg) : m_alg(alg)
{
}

Cipher::~Cipher()
{
}

size_t
Cipher::block_size() const
{
    return pgp_block_size(m_alg);
}
