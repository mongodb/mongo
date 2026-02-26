/*
 * Copyright (c) 2023, [MTG AG](https://www.mtg.de).
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

#if defined(ENABLE_CRYPTO_REFRESH)

#include "hkdf_botan.hpp"
#include "hash_botan.hpp"

namespace rnp {

Hkdf_Botan::Hkdf_Botan(pgp_hash_alg_t hash_alg) : Hkdf(hash_alg)
{
}

std::unique_ptr<Hkdf_Botan>
Hkdf_Botan::create(pgp_hash_alg_t alg)
{
    return std::unique_ptr<Hkdf_Botan>(new Hkdf_Botan(alg));
}

std::string
Hkdf_Botan::alg() const
{
    return std::string("HKDF(") + Hash_Botan::name_backend(Hkdf::alg()) + ")";
}

void
Hkdf_Botan::extract_expand(const uint8_t *salt,
                           size_t         salt_len,
                           const uint8_t *ikm,
                           size_t         ikm_len,
                           const uint8_t *info,
                           size_t         info_len,
                           uint8_t *      output_buf,
                           size_t         output_length)
{
    std::unique_ptr<Botan::KDF> kdf = Botan::KDF::create_or_throw(Hkdf_Botan::alg(), "");

    Botan::secure_vector<uint8_t> OKM;
    OKM = kdf->derive_key(output_length, ikm, ikm_len, salt, salt_len, info, info_len);

    memcpy(output_buf, Botan::unlock(OKM).data(), output_length);
}

Hkdf_Botan::~Hkdf_Botan()
{
}

} // namespace rnp

#endif
