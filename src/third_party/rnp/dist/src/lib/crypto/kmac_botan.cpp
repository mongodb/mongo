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

#include "kmac_botan.hpp"
#include "hash_botan.hpp"
#include "botan/mac.h"

#if defined(ENABLE_PQC_DBG_LOG)
#include "crypto/mem.h"
#endif

namespace rnp {

KMAC256_Botan::KMAC256_Botan() : KMAC256()
{
}

std::unique_ptr<KMAC256_Botan>
KMAC256_Botan::create()
{
    return std::unique_ptr<KMAC256_Botan>(new KMAC256_Botan());
}

void
KMAC256_Botan::compute(const std::vector<uint8_t> &ecc_key_share,
                       const std::vector<uint8_t> &ecc_ciphertext,
                       const std::vector<uint8_t> &kyber_key_share,
                       const std::vector<uint8_t> &kyber_ciphertext,
                       const pgp_pubkey_alg_t      alg_id,
                       std::vector<uint8_t> &      out)
{
    auto kmac = Botan::MessageAuthenticationCode::create_or_throw("KMAC-256(256)");

    /* the mapping between the KEM Combiner and the MAC interface is:
     * key     <> domSeparation
     * nonce   <> customizationString
     * message <> encData
     */

#if defined(ENABLE_PQC_DBG_LOG)
    RNP_LOG_U8VEC("KMAC256 domSeparation: %s", domSeparation());
    RNP_LOG_U8VEC("KMAC256 customizationString: %s", customizationString());
#endif

    kmac->set_key(domSeparation());
    kmac->start(customizationString()); // set nonce
    kmac->update(
      encData(ecc_key_share, ecc_ciphertext, kyber_key_share, kyber_ciphertext, alg_id));
    out = kmac->final_stdvec();

#if defined(ENABLE_PQC_DBG_LOG)
    RNP_LOG_U8VEC("KMAC256 Output: %s", out);
#endif
}

KMAC256_Botan::~KMAC256_Botan()
{
}

} // namespace rnp
