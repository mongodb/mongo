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

#include "v2_seipd.h"
#include "logging.h"
#include "crypto/hkdf.hpp"

seipd_v2_aead_fields_t
seipd_v2_key_and_nonce_derivation(pgp_seipdv2_hdr_t &hdr, uint8_t *sesskey)
{
    auto hkdf = rnp::Hkdf::create(PGP_HASH_SHA256);

    size_t   sesskey_len = pgp_key_size(hdr.cipher_alg);
    uint32_t nonce_size = 0;
    uint32_t key_size = pgp_key_size(hdr.cipher_alg);

    switch (hdr.aead_alg) {
    case PGP_AEAD_EAX:
        nonce_size = 16;
        break;
    case PGP_AEAD_OCB:
        nonce_size = 15;
        break;
    case PGP_AEAD_NONE:
    case PGP_AEAD_UNKNOWN:
        RNP_LOG("only EAX and OCB is supported for v2 SEIPD packets");
        throw rnp::rnp_exception(RNP_ERROR_NOT_SUPPORTED);
    }
    const uint8_t info[5] = {
      static_cast<uint8_t>(PGP_PKT_SE_IP_DATA | PGP_PTAG_ALWAYS_SET | PGP_PTAG_NEW_FORMAT),
      static_cast<uint8_t>(hdr.version),
      static_cast<uint8_t>(hdr.cipher_alg),
      static_cast<uint8_t>(hdr.aead_alg),
      hdr.chunk_size_octet};
    uint32_t             out_size = key_size + nonce_size - 8;
    std::vector<uint8_t> hkdf_out(out_size);
    hkdf->extract_expand(hdr.salt,
                         sizeof(hdr.salt),
                         sesskey,
                         sesskey_len,
                         info,
                         sizeof(info),
                         hkdf_out.data(),
                         hkdf_out.size());

    seipd_v2_aead_fields_t result;
    result.key = std::vector<uint8_t>(hkdf_out.data(), hkdf_out.data() + key_size);
    result.nonce =
      std::vector<uint8_t>(hkdf_out.data() + key_size, hkdf_out.data() + hkdf_out.size());
    for (uint32_t i = 0; i < 8; i++) {
        // fill up the nonce with zero octets
        result.nonce.push_back(0);
    }
    return result;
}

#endif
