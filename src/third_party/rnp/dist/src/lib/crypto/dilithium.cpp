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

#include "dilithium.h"
#include <cassert>

namespace {

Botan::DilithiumMode
rnp_dilithium_param_to_botan_dimension(dilithium_parameter_e mode)
{
    Botan::DilithiumMode result = Botan::DilithiumMode::Dilithium8x7;
    if (mode == dilithium_parameter_e::dilithium_L3) {
        result = Botan::DilithiumMode::Dilithium6x5;
    }
    return result;
}

} // namespace

std::vector<uint8_t>
pgp_dilithium_private_key_t::sign(rnp::RNG *rng, const uint8_t *msg, size_t msg_len) const
{
    assert(is_initialized_);
    auto priv_key = botan_key();

    auto                 signer = Botan::PK_Signer(priv_key, *rng->obj(), "");
    std::vector<uint8_t> signature = signer.sign_message(msg, msg_len, *rng->obj());
    // std::vector<uint8_t> signature;

    return signature;
}

Botan::Dilithium_PublicKey
pgp_dilithium_public_key_t::botan_key() const
{
    return Botan::Dilithium_PublicKey(
      key_encoded_, rnp_dilithium_param_to_botan_dimension(dilithium_param_));
}

Botan::Dilithium_PrivateKey
pgp_dilithium_private_key_t::botan_key() const
{
    Botan::secure_vector<uint8_t> priv_sv(key_encoded_.data(),
                                          key_encoded_.data() + key_encoded_.size());
    return Botan::Dilithium_PrivateKey(
      priv_sv, rnp_dilithium_param_to_botan_dimension(this->dilithium_param_));
}

bool
pgp_dilithium_public_key_t::verify_signature(const uint8_t *msg,
                                             size_t         msg_len,
                                             const uint8_t *signature,
                                             size_t         signature_len) const
{
    assert(is_initialized_);
    auto pub_key = botan_key();

    auto verificator = Botan::PK_Verifier(pub_key, "");
    return verificator.verify_message(msg, msg_len, signature, signature_len);
}

std::pair<pgp_dilithium_public_key_t, pgp_dilithium_private_key_t>
dilithium_generate_keypair(rnp::RNG *rng, dilithium_parameter_e dilithium_param)
{
    Botan::Dilithium_PrivateKey priv_key(
      *rng->obj(), rnp_dilithium_param_to_botan_dimension(dilithium_param));

    std::unique_ptr<Botan::Public_Key> pub_key = priv_key.public_key();
    Botan::secure_vector<uint8_t>      priv_bits = priv_key.private_key_bits();
    return std::make_pair(
      pgp_dilithium_public_key_t(pub_key->public_key_bits(), dilithium_param),
      pgp_dilithium_private_key_t(priv_bits.data(), priv_bits.size(), dilithium_param));
}

bool
pgp_dilithium_public_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized_) {
        return false;
    }

    auto key = botan_key();
    return key.check_key(*(rng->obj()), false);
}

bool
pgp_dilithium_private_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized_) {
        return false;
    }

    auto key = botan_key();
    return key.check_key(*(rng->obj()), false);
}

bool
dilithium_hash_allowed(pgp_hash_alg_t hash_alg)
{
    switch (hash_alg) {
    case PGP_HASH_SHA3_256:
    case PGP_HASH_SHA3_512:
        return true;
    default:
        return false;
    }
}

pgp_hash_alg_t
dilithium_default_hash_alg()
{
    return PGP_HASH_SHA3_256;
}