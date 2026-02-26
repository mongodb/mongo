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

#include "sphincsplus.h"
#include <cassert>
#include "logging.h"
#include "types.h"

namespace {
Botan::Sphincs_Parameter_Set
rnp_sphincsplus_params_to_botan_param(sphincsplus_parameter_t param)
{
    switch (param) {
    case sphincsplus_simple_128s:
        return Botan::Sphincs_Parameter_Set::Sphincs128Small;
    case sphincsplus_simple_128f:
        return Botan::Sphincs_Parameter_Set::Sphincs128Fast;
    case sphincsplus_simple_192s:
        return Botan::Sphincs_Parameter_Set::Sphincs192Small;
    case sphincsplus_simple_192f:
        return Botan::Sphincs_Parameter_Set::Sphincs192Fast;
    case sphincsplus_simple_256s:
        return Botan::Sphincs_Parameter_Set::Sphincs256Small;
    case sphincsplus_simple_256f:
        return Botan::Sphincs_Parameter_Set::Sphincs256Fast;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}
Botan::Sphincs_Hash_Type
rnp_sphincsplus_hash_func_to_botan_hash_func(sphincsplus_hash_func_t hash_func)
{
    switch (hash_func) {
    case sphincsplus_sha256:
        return Botan::Sphincs_Hash_Type::Sha256;
    case sphinscplus_shake256:
        return Botan::Sphincs_Hash_Type::Shake256;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

sphincsplus_hash_func_t
rnp_sphincsplus_alg_to_hashfunc(pgp_pubkey_alg_t alg)
{
    switch (alg) {
    case PGP_PKA_SPHINCSPLUS_SHA2:
        return sphincsplus_sha256;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        return sphinscplus_shake256;
    default:
        RNP_LOG("invalid SLH-DSA alg id");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

pgp_pubkey_alg_t
rnp_sphincsplus_hashfunc_to_alg(sphincsplus_hash_func_t hashfunc)
{
    switch (hashfunc) {
    case sphincsplus_sha256:
        return PGP_PKA_SPHINCSPLUS_SHA2;
    case sphinscplus_shake256:
        return PGP_PKA_SPHINCSPLUS_SHAKE;
    default:
        RNP_LOG("invalid SLH-DSA hashfunc");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}
} // namespace

pgp_sphincsplus_public_key_t::pgp_sphincsplus_public_key_t(const uint8_t *key_encoded,
                                                           size_t         key_encoded_len,
                                                           sphincsplus_parameter_t param,
                                                           sphincsplus_hash_func_t hash_func)
    : key_encoded_(key_encoded, key_encoded + key_encoded_len),
      pk_alg_(rnp_sphincsplus_hashfunc_to_alg(hash_func)), sphincsplus_param_(param),
      sphincsplus_hash_func_(hash_func), is_initialized_(true)
{
}

pgp_sphincsplus_public_key_t::pgp_sphincsplus_public_key_t(
  std::vector<uint8_t> const &key_encoded,
  sphincsplus_parameter_t     param,
  sphincsplus_hash_func_t     hash_func)
    : key_encoded_(key_encoded), pk_alg_(rnp_sphincsplus_hashfunc_to_alg(hash_func)),
      sphincsplus_param_(param), sphincsplus_hash_func_(hash_func), is_initialized_(true)
{
}

pgp_sphincsplus_public_key_t::pgp_sphincsplus_public_key_t(
  std::vector<uint8_t> const &key_encoded, sphincsplus_parameter_t param, pgp_pubkey_alg_t alg)
    : key_encoded_(key_encoded), pk_alg_(alg), sphincsplus_param_(param),
      sphincsplus_hash_func_(rnp_sphincsplus_alg_to_hashfunc(alg)), is_initialized_(true)
{
}

pgp_sphincsplus_private_key_t::pgp_sphincsplus_private_key_t(const uint8_t *key_encoded,
                                                             size_t         key_encoded_len,
                                                             sphincsplus_parameter_t param,
                                                             sphincsplus_hash_func_t hash_func)
    : key_encoded_(key_encoded, key_encoded + key_encoded_len),
      pk_alg_(rnp_sphincsplus_hashfunc_to_alg(hash_func)), sphincsplus_param_(param),
      sphincsplus_hash_func_(hash_func), is_initialized_(true)
{
}

pgp_sphincsplus_private_key_t::pgp_sphincsplus_private_key_t(
  std::vector<uint8_t> const &key_encoded,
  sphincsplus_parameter_t     param,
  sphincsplus_hash_func_t     hash_func)
    : key_encoded_(Botan::secure_vector<uint8_t>(key_encoded.begin(), key_encoded.end())),
      pk_alg_(rnp_sphincsplus_hashfunc_to_alg(hash_func)), sphincsplus_param_(param),
      sphincsplus_hash_func_(hash_func), is_initialized_(true)
{
}

pgp_sphincsplus_private_key_t::pgp_sphincsplus_private_key_t(
  std::vector<uint8_t> const &key_encoded, sphincsplus_parameter_t param, pgp_pubkey_alg_t alg)
    : key_encoded_(Botan::secure_vector<uint8_t>(key_encoded.begin(), key_encoded.end())),
      pk_alg_(alg), sphincsplus_param_(param),
      sphincsplus_hash_func_(rnp_sphincsplus_alg_to_hashfunc(alg)), is_initialized_(true)
{
}

rnp_result_t
pgp_sphincsplus_private_key_t::sign(rnp::RNG *                   rng,
                                    pgp_sphincsplus_signature_t *sig,
                                    const uint8_t *              msg,
                                    size_t                       msg_len) const
{
    assert(is_initialized_);
    auto priv_key = botan_key();

    auto signer = Botan::PK_Signer(priv_key, *rng->obj(), "");
    sig->sig = signer.sign_message(msg, msg_len, *rng->obj());
    sig->param = param();

    return RNP_SUCCESS;
}

Botan::SphincsPlus_PublicKey
pgp_sphincsplus_public_key_t::botan_key() const
{
    return Botan::SphincsPlus_PublicKey(
      key_encoded_,
      rnp_sphincsplus_params_to_botan_param(this->sphincsplus_param_),
      rnp_sphincsplus_hash_func_to_botan_hash_func(this->sphincsplus_hash_func_));
}

Botan::SphincsPlus_PrivateKey
pgp_sphincsplus_private_key_t::botan_key() const
{
    Botan::secure_vector<uint8_t> priv_sv(key_encoded_.data(),
                                          key_encoded_.data() + key_encoded_.size());
    return Botan::SphincsPlus_PrivateKey(
      priv_sv,
      rnp_sphincsplus_params_to_botan_param(this->sphincsplus_param_),
      rnp_sphincsplus_hash_func_to_botan_hash_func(this->sphincsplus_hash_func_));
}

rnp_result_t
pgp_sphincsplus_public_key_t::verify(const pgp_sphincsplus_signature_t *sig,
                                     const uint8_t *                    msg,
                                     size_t                             msg_len) const
{
    assert(is_initialized_);
    auto pub_key = botan_key();

    auto verificator = Botan::PK_Verifier(pub_key, "");
    if (verificator.verify_message(msg, msg_len, sig->sig.data(), sig->sig.size())) {
        return RNP_SUCCESS;
    }
    return RNP_ERROR_SIGNATURE_INVALID;
}

std::pair<pgp_sphincsplus_public_key_t, pgp_sphincsplus_private_key_t>
sphincsplus_generate_keypair(rnp::RNG *              rng,
                             sphincsplus_parameter_t sphincsplus_param,
                             sphincsplus_hash_func_t sphincsplus_hash_func)
{
    Botan::SphincsPlus_PrivateKey priv_key(
      *rng->obj(),
      rnp_sphincsplus_params_to_botan_param(sphincsplus_param),
      rnp_sphincsplus_hash_func_to_botan_hash_func(sphincsplus_hash_func));

    std::unique_ptr<Botan::Public_Key> pub_key = priv_key.public_key();
    Botan::secure_vector<uint8_t>      priv_bits = priv_key.private_key_bits();
    return std::make_pair(
      pgp_sphincsplus_public_key_t(
        pub_key->public_key_bits(), sphincsplus_param, sphincsplus_hash_func),
      pgp_sphincsplus_private_key_t(
        priv_bits.data(), priv_bits.size(), sphincsplus_param, sphincsplus_hash_func));
}

rnp_result_t
pgp_sphincsplus_generate(rnp::RNG *              rng,
                         pgp_sphincsplus_key_t * material,
                         sphincsplus_parameter_t param,
                         pgp_pubkey_alg_t        alg)
{
    auto keypair =
      sphincsplus_generate_keypair(rng, param, rnp_sphincsplus_alg_to_hashfunc(alg));
    material->pub = keypair.first;
    material->priv = keypair.second;

    return RNP_SUCCESS;
}

bool
pgp_sphincsplus_public_key_t::validate_signature_hash_requirements(
  pgp_hash_alg_t hash_alg) const
{
    /* check if key is allowed with the hash algorithm */
    return sphincsplus_hash_allowed(pk_alg_, sphincsplus_param_, hash_alg);
}

bool
pgp_sphincsplus_public_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized_) {
        return false;
    }

    auto key = botan_key();
    return key.check_key(*(rng->obj()), false);
}

bool
pgp_sphincsplus_private_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized_) {
        return false;
    }

    auto key = botan_key();
    return key.check_key(*(rng->obj()), false);
}

rnp_result_t
sphincsplus_validate_key(rnp::RNG *rng, const pgp_sphincsplus_key_t *key, bool secret)
{
    bool valid;

    valid = key->pub.is_valid(rng);
    if (secret) {
        valid = valid && key->priv.is_valid(rng);
    }
    if (!valid) {
        return RNP_ERROR_GENERIC;
    }

    return RNP_SUCCESS;
}

size_t
sphincsplus_privkey_size(sphincsplus_parameter_t param)
{
    return 2 * sphincsplus_pubkey_size(param);
}

size_t
sphincsplus_pubkey_size(sphincsplus_parameter_t param)
{
    switch (param) {
    case sphincsplus_simple_128s:
        return 32;
    case sphincsplus_simple_128f:
        return 32;
    case sphincsplus_simple_192s:
        return 48;
    case sphincsplus_simple_192f:
        return 48;
    case sphincsplus_simple_256s:
        return 64;
    case sphincsplus_simple_256f:
        return 64;
    default:
        RNP_LOG("invalid SLH-DSA parameter identifier");
        return 0;
    }
}

size_t
sphincsplus_signature_size(sphincsplus_parameter_t param)
{
    switch (param) {
    case sphincsplus_simple_128s:
        return 7856;
    case sphincsplus_simple_128f:
        return 17088;
    case sphincsplus_simple_192s:
        return 16224;
    case sphincsplus_simple_192f:
        return 35664;
    case sphincsplus_simple_256s:
        return 29792;
    case sphincsplus_simple_256f:
        return 49856;
    default:
        RNP_LOG("invalid SLH-DSA parameter identifier");
        return 0;
    }
}

bool
sphincsplus_hash_allowed(pgp_pubkey_alg_t        pk_alg,
                         sphincsplus_parameter_t sphincsplus_param,
                         pgp_hash_alg_t          hash_alg)
{
    /* draft-wussler-openpgp-pqc-02 Table 14*/
    switch (pk_alg) {
    case PGP_PKA_SPHINCSPLUS_SHA2:
        switch (sphincsplus_param) {
        case sphincsplus_simple_128s:
            FALLTHROUGH_STATEMENT;
        case sphincsplus_simple_128f:
            if (hash_alg != PGP_HASH_SHA256) {
                return false;
            }
            break;
        case sphincsplus_simple_192s:
            FALLTHROUGH_STATEMENT;
        case sphincsplus_simple_192f:
            FALLTHROUGH_STATEMENT;
        case sphincsplus_simple_256s:
            FALLTHROUGH_STATEMENT;
        case sphincsplus_simple_256f:
            if (hash_alg != PGP_HASH_SHA512) {
                return false;
            }
            break;
        }
        break;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        switch (sphincsplus_param) {
        case sphincsplus_simple_128s:
            FALLTHROUGH_STATEMENT;
        case sphincsplus_simple_128f:
            if (hash_alg != PGP_HASH_SHA3_256) {
                return false;
            }
            break;
        case sphincsplus_simple_192s:
            FALLTHROUGH_STATEMENT;
        case sphincsplus_simple_192f:
            FALLTHROUGH_STATEMENT;
        case sphincsplus_simple_256s:
            FALLTHROUGH_STATEMENT;
        case sphincsplus_simple_256f:
            if (hash_alg != PGP_HASH_SHA3_512) {
                return false;
            }
            break;
        }
        break;
    default:
        break;
    }
    return true;
}

pgp_hash_alg_t
sphincsplus_default_hash_alg(pgp_pubkey_alg_t        pk_alg,
                             sphincsplus_parameter_t sphincsplus_param)
{
    switch (sphincsplus_param) {
    case sphincsplus_simple_128s:
        FALLTHROUGH_STATEMENT;
    case sphincsplus_simple_128f:
        switch (pk_alg) {
        case PGP_PKA_SPHINCSPLUS_SHA2:
            return PGP_HASH_SHA256;
        case PGP_PKA_SPHINCSPLUS_SHAKE:
            return PGP_HASH_SHA3_256;
        default:
            RNP_LOG("invalid parameter given");
            throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
        }
    case sphincsplus_simple_192s:
        FALLTHROUGH_STATEMENT;
    case sphincsplus_simple_192f:
        FALLTHROUGH_STATEMENT;
    case sphincsplus_simple_256s:
        FALLTHROUGH_STATEMENT;
    case sphincsplus_simple_256f:
        switch (pk_alg) {
        case PGP_PKA_SPHINCSPLUS_SHA2:
            return PGP_HASH_SHA512;
        case PGP_PKA_SPHINCSPLUS_SHAKE:
            return PGP_HASH_SHA3_512;
        default:
            RNP_LOG("invalid parameter given");
            throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
        }
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}
