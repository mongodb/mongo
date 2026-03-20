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

#include "kyber.h"
#include <utility>
#include <vector>
#include <cassert>

namespace {
Botan::KyberMode
rnp_kyber_param_to_botan_kyber_mode(kyber_parameter_e mode)
{
#if defined(BOTAN_HAS_ML_KEM_INITIAL_PUBLIC_DRAFT) && defined(ENABLE_PQC_MLKEM_IPD)
    Botan::KyberMode result = Botan::KyberMode::ML_KEM_1024_ipd;
    if (mode == kyber_768) {
        result = Botan::KyberMode::ML_KEM_768_ipd;
    }
#else
    Botan::KyberMode result = Botan::KyberMode::Kyber1024;
    if (mode == kyber_768) {
        result = Botan::KyberMode::Kyber768;
    }
#endif
    return result;
}

uint32_t
key_share_size_from_kyber_param(kyber_parameter_e param)
{
    if (param == kyber_768) {
        return 24;
    }
    return 32; // kyber_1024
}
} // namespace

std::pair<pgp_kyber_public_key_t, pgp_kyber_private_key_t>
kyber_generate_keypair(rnp::RNG *rng, kyber_parameter_e kyber_param)
{
    Botan::Kyber_PrivateKey kyber_priv(*rng->obj(),
                                       rnp_kyber_param_to_botan_kyber_mode(kyber_param));

    Botan::secure_vector<uint8_t>      encoded_private_key = kyber_priv.private_key_bits();
    std::unique_ptr<Botan::Public_Key> kyber_pub = kyber_priv.public_key();

    std::vector<uint8_t> encoded_public_key = kyber_priv.public_key_bits();
    return std::make_pair(pgp_kyber_public_key_t(encoded_public_key, kyber_param),
                          pgp_kyber_private_key_t(encoded_private_key.data(),
                                                  encoded_private_key.size(),
                                                  kyber_param));
}

Botan::Kyber_PublicKey
pgp_kyber_public_key_t::botan_key() const
{
    return Botan::Kyber_PublicKey(key_encoded_,
                                  rnp_kyber_param_to_botan_kyber_mode(kyber_mode_));
}

Botan::Kyber_PrivateKey
pgp_kyber_private_key_t::botan_key() const
{
    Botan::secure_vector<uint8_t> key_sv(key_encoded_.data(),
                                         key_encoded_.data() + key_encoded_.size());
    return Botan::Kyber_PrivateKey(key_sv, rnp_kyber_param_to_botan_kyber_mode(kyber_mode_));
}

kyber_encap_result_t
pgp_kyber_public_key_t::encapsulate(rnp::RNG *rng) const
{
    assert(is_initialized_);
    auto decoded_kyber_pub = botan_key();

    Botan::PK_KEM_Encryptor       kem_enc(decoded_kyber_pub, "Raw", "base");
    Botan::secure_vector<uint8_t> encap_key;           // this has to go over the wire
    Botan::secure_vector<uint8_t> data_encryption_key; // this is the key used for
    // encryption of the payload data
    kem_enc.encrypt(encap_key,
                    data_encryption_key,
                    *rng->obj(),
                    key_share_size_from_kyber_param(kyber_mode_));
    kyber_encap_result_t result;
    result.ciphertext.insert(
      result.ciphertext.end(), encap_key.data(), encap_key.data() + encap_key.size());
    result.symmetric_key.insert(result.symmetric_key.end(),
                                data_encryption_key.data(),
                                data_encryption_key.data() + data_encryption_key.size());
    return result;
}

std::vector<uint8_t>
pgp_kyber_private_key_t::decapsulate(rnp::RNG *     rng,
                                     const uint8_t *ciphertext,
                                     size_t         ciphertext_len)
{
    assert(is_initialized_);
    auto                          decoded_kyber_priv = botan_key();
    Botan::PK_KEM_Decryptor       kem_dec(decoded_kyber_priv, *rng->obj(), "Raw", "base");
    Botan::secure_vector<uint8_t> dec_shared_key = kem_dec.decrypt(
      ciphertext, ciphertext_len, key_share_size_from_kyber_param(kyber_mode_));
    return std::vector<uint8_t>(dec_shared_key.data(),
                                dec_shared_key.data() + dec_shared_key.size());
}

bool
pgp_kyber_public_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized_) {
        return false;
    }

    auto key = botan_key();
    return key.check_key(*(rng->obj()), false);
}

bool
pgp_kyber_private_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized_) {
        return false;
    }

    auto key = botan_key();
    return key.check_key(*(rng->obj()), false);
}
