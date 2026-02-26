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

#include "dilithium_exdsa_composite.h"
#include "types.h"
#include "logging.h"

pgp_dilithium_exdsa_composite_key_t::~pgp_dilithium_exdsa_composite_key_t()
{
}

void
pgp_dilithium_exdsa_composite_key_t::initialized_or_throw() const
{
    if (!is_initialized()) {
        RNP_LOG("Trying to use uninitialized mldsa-ecdsa/eddsa key");
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }
}

rnp_result_t
pgp_dilithium_exdsa_composite_key_t::gen_keypair(rnp::RNG *                 rng,
                                                 pgp_dilithium_exdsa_key_t *key,
                                                 pgp_pubkey_alg_t           alg)
{
    rnp_result_t          res;
    pgp_curve_t           curve = pk_alg_to_curve_id(alg);
    dilithium_parameter_e dilithium_id = pk_alg_to_dilithium_id(alg);

    exdsa_key_t exdsa_key_pair;

    res = ec_key_t::generate_exdsa_key_pair(rng, &exdsa_key_pair, curve);
    if (res != RNP_SUCCESS) {
        RNP_LOG("generating mldsa exdsa composite key failed when generating exdsa key");
        return res;
    }

    auto dilithium_key_pair = dilithium_generate_keypair(rng, dilithium_id);

    key->priv = pgp_dilithium_exdsa_composite_private_key_t(
      exdsa_key_pair.priv.get_encoded(), dilithium_key_pair.second.get_encoded(), alg);
    key->pub = pgp_dilithium_exdsa_composite_public_key_t(
      exdsa_key_pair.pub.get_encoded(), dilithium_key_pair.first.get_encoded(), alg);

    return RNP_SUCCESS;
}

size_t
pgp_dilithium_exdsa_composite_key_t::exdsa_curve_privkey_size(pgp_curve_t curve)
{
    switch (curve) {
    case PGP_CURVE_ED25519:
        return 32;
    /* TODO */
    // case PGP_CURVE_ED448:
    //   return 56;
    case PGP_CURVE_NIST_P_256:
        return 32;
    case PGP_CURVE_NIST_P_384:
        return 48;
    case PGP_CURVE_BP256:
        return 32;
    case PGP_CURVE_BP384:
        return 48;
    default:
        RNP_LOG("invalid curve given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

size_t
pgp_dilithium_exdsa_composite_key_t::exdsa_curve_pubkey_size(pgp_curve_t curve)
{
    switch (curve) {
    case PGP_CURVE_ED25519:
        return 32;
    /* TODO */
    //  case PGP_CURVE_ED448:
    //    return 56;
    case PGP_CURVE_NIST_P_256:
        return 65;
    case PGP_CURVE_NIST_P_384:
        return 97;
    case PGP_CURVE_BP256:
        return 65;
    case PGP_CURVE_BP384:
        return 97;
    default:
        RNP_LOG("invalid curve given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

size_t
pgp_dilithium_exdsa_composite_key_t::exdsa_curve_signature_size(pgp_curve_t curve)
{
    switch (curve) {
    case PGP_CURVE_ED25519:
        return 64;
    /* TODO */
    //  case PGP_CURVE_ED448:
    //    return 114;
    case PGP_CURVE_NIST_P_256:
        return 64;
    case PGP_CURVE_NIST_P_384:
        return 96;
    case PGP_CURVE_BP256:
        return 64;
    case PGP_CURVE_BP384:
        return 96;
    default:
        RNP_LOG("invalid curve given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

dilithium_parameter_e
pgp_dilithium_exdsa_composite_key_t::pk_alg_to_dilithium_id(pgp_pubkey_alg_t pk_alg)
{
    switch (pk_alg) {
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        return dilithium_L3;
    case PGP_PKA_DILITHIUM5_BP384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        return dilithium_L5;
    default:
        RNP_LOG("invalid PK alg given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

pgp_curve_t
pgp_dilithium_exdsa_composite_key_t::pk_alg_to_curve_id(pgp_pubkey_alg_t pk_alg)
{
    switch (pk_alg) {
    case PGP_PKA_DILITHIUM3_ED25519:
        return PGP_CURVE_ED25519;
    case PGP_PKA_DILITHIUM3_P256:
        return PGP_CURVE_NIST_P_256;
    case PGP_PKA_DILITHIUM3_BP256:
        return PGP_CURVE_BP256;
    case PGP_PKA_DILITHIUM5_BP384:
        return PGP_CURVE_BP384;
    case PGP_PKA_DILITHIUM5_P384:
        return PGP_CURVE_NIST_P_384;
    /*case PGP_PKA_DILITHIUM5_ED448:
      throw rnp::rnp_exception(RNP_ERROR_NOT_IMPLEMENTED);*/
    default:
        RNP_LOG("invalid PK alg given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

pgp_dilithium_exdsa_composite_public_key_t::pgp_dilithium_exdsa_composite_public_key_t(
  const uint8_t *key_encoded, size_t key_encoded_len, pgp_pubkey_alg_t pk_alg)
    : pk_alg_(pk_alg)
{
    parse_component_keys(std::vector<uint8_t>(key_encoded, key_encoded + key_encoded_len));
}

pgp_dilithium_exdsa_composite_public_key_t::pgp_dilithium_exdsa_composite_public_key_t(
  std::vector<uint8_t> const &key_encoded, pgp_pubkey_alg_t pk_alg)
    : pk_alg_(pk_alg)
{
    parse_component_keys(key_encoded);
}

pgp_dilithium_exdsa_composite_public_key_t::pgp_dilithium_exdsa_composite_public_key_t(
  std::vector<uint8_t> const &exdsa_key_encoded,
  std::vector<uint8_t> const &dilithium_key_encoded,
  pgp_pubkey_alg_t            pk_alg)
    : pk_alg_(pk_alg), dilithium_key_(dilithium_key_encoded, pk_alg_to_dilithium_id(pk_alg)),
      exdsa_key_(exdsa_key_encoded, pk_alg_to_curve_id(pk_alg))
{
    if (exdsa_curve_pubkey_size(pk_alg_to_curve_id(pk_alg)) != exdsa_key_encoded.size() ||
        dilithium_pubkey_size(pk_alg_to_dilithium_id(pk_alg)) !=
          dilithium_key_encoded.size()) {
        RNP_LOG("exdsa or mldsa key length mismatch");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    is_initialized_ = true;
}

pgp_dilithium_exdsa_composite_private_key_t::pgp_dilithium_exdsa_composite_private_key_t(
  const uint8_t *key_encoded, size_t key_encoded_len, pgp_pubkey_alg_t pk_alg)
    : pk_alg_(pk_alg)
{
    parse_component_keys(std::vector<uint8_t>(key_encoded, key_encoded + key_encoded_len));
}

pgp_dilithium_exdsa_composite_private_key_t::pgp_dilithium_exdsa_composite_private_key_t(
  std::vector<uint8_t> const &key_encoded, pgp_pubkey_alg_t pk_alg)
    : pk_alg_(pk_alg)
{
    parse_component_keys(key_encoded);
}

/* copy assignment operator is used on key materials struct and thus needs to be defined for
 * this class as well */
pgp_dilithium_exdsa_composite_private_key_t &
pgp_dilithium_exdsa_composite_private_key_t::operator=(
  const pgp_dilithium_exdsa_composite_private_key_t &other)
{
    pgp_dilithium_exdsa_composite_key_t::operator=(other);
    pk_alg_ = other.pk_alg_;
    if (other.is_initialized() && other.dilithium_key_) {
        dilithium_key_ =
          std::make_unique<pgp_dilithium_private_key_t>(pgp_dilithium_private_key_t(
            other.dilithium_key_->get_encoded(), other.dilithium_key_->param()));
    }
    if (other.is_initialized() && other.exdsa_key_) {
        exdsa_key_ = std::make_unique<exdsa_private_key_t>(
          exdsa_private_key_t(other.exdsa_key_->get_encoded(), other.exdsa_key_->get_curve()));
    }

    return *this;
}

pgp_dilithium_exdsa_composite_private_key_t::pgp_dilithium_exdsa_composite_private_key_t(
  const pgp_dilithium_exdsa_composite_private_key_t &other)
{
    *this = other;
}

pgp_dilithium_exdsa_composite_private_key_t::pgp_dilithium_exdsa_composite_private_key_t(
  std::vector<uint8_t> const &exdsa_key_encoded,
  std::vector<uint8_t> const &dilithium_key_encoded,
  pgp_pubkey_alg_t            pk_alg)
    : pk_alg_(pk_alg)
{
    if (exdsa_curve_privkey_size(pk_alg_to_curve_id(pk_alg)) != exdsa_key_encoded.size() ||
        dilithium_privkey_size(pk_alg_to_dilithium_id(pk_alg)) !=
          dilithium_key_encoded.size()) {
        RNP_LOG("exdsa or mldsa key length mismatch");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    dilithium_key_ = std::make_unique<pgp_dilithium_private_key_t>(
      pgp_dilithium_private_key_t(dilithium_key_encoded, pk_alg_to_dilithium_id(pk_alg)));
    exdsa_key_ = std::make_unique<exdsa_private_key_t>(
      exdsa_private_key_t(exdsa_key_encoded, pk_alg_to_curve_id(pk_alg)));
    is_initialized_ = true;
}

size_t
pgp_dilithium_exdsa_composite_private_key_t::encoded_size(pgp_pubkey_alg_t pk_alg)
{
    dilithium_parameter_e dilithium_param = pk_alg_to_dilithium_id(pk_alg);
    pgp_curve_t           curve = pk_alg_to_curve_id(pk_alg);
    return exdsa_curve_privkey_size(curve) + dilithium_privkey_size(dilithium_param);
}

void
pgp_dilithium_exdsa_composite_private_key_t::parse_component_keys(
  std::vector<uint8_t> key_encoded)
{
    if (key_encoded.size() != encoded_size(pk_alg_)) {
        RNP_LOG("ML-DSA composite key format invalid: length mismatch");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    dilithium_parameter_e dilithium_param = pk_alg_to_dilithium_id(pk_alg_);
    pgp_curve_t           curve = pk_alg_to_curve_id(pk_alg_);
    size_t                split_at = exdsa_curve_privkey_size(pk_alg_to_curve_id(pk_alg_));

    dilithium_key_ = std::make_unique<pgp_dilithium_private_key_t>(pgp_dilithium_private_key_t(
      key_encoded.data() + split_at, key_encoded.size() - split_at, dilithium_param));
    exdsa_key_ = std::make_unique<exdsa_private_key_t>(
      exdsa_private_key_t(key_encoded.data(), split_at, curve));

    is_initialized_ = true;
}

std::vector<uint8_t>
pgp_dilithium_exdsa_composite_private_key_t::get_encoded() const
{
    initialized_or_throw();
    std::vector<uint8_t> result;
    std::vector<uint8_t> exdsa_key_encoded = exdsa_key_->get_encoded();
    std::vector<uint8_t> dilithium_key_encoded = dilithium_key_->get_encoded();

    result.insert(result.end(), std::begin(exdsa_key_encoded), std::end(exdsa_key_encoded));
    result.insert(
      result.end(), std::begin(dilithium_key_encoded), std::end(dilithium_key_encoded));
    return result;
};

rnp_result_t
pgp_dilithium_exdsa_composite_private_key_t::sign(rnp::RNG *                       rng,
                                                  pgp_dilithium_exdsa_signature_t *sig,
                                                  pgp_hash_alg_t                   hash_alg,
                                                  const uint8_t *                  msg,
                                                  size_t msg_len) const
{
    initialized_or_throw();
    std::vector<uint8_t> dilithium_sig;
    std::vector<uint8_t> exdsa_sig;
    rnp_result_t         ret;

    try {
        dilithium_sig = dilithium_key_->sign(rng, msg, msg_len);
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return RNP_ERROR_SIGNING_FAILED;
    }
    ret = exdsa_key_->sign(rng, exdsa_sig, msg, msg_len, hash_alg);
    if (ret != RNP_SUCCESS) {
        RNP_LOG("exdsa sign failed");
        return RNP_ERROR_SIGNING_FAILED;
    }

    sig->sig.assign(exdsa_sig.data(), exdsa_sig.data() + exdsa_sig.size());
    sig->sig.insert(sig->sig.end(), dilithium_sig.begin(), dilithium_sig.end());

    return RNP_SUCCESS;
}

void
pgp_dilithium_exdsa_composite_private_key_t::secure_clear()
{
    // private key buffer is stored in a secure_vector and will be securely erased by the
    // destructor.
    dilithium_key_.reset();
    exdsa_key_.reset();
    is_initialized_ = false;
}

size_t
pgp_dilithium_exdsa_composite_public_key_t::encoded_size(pgp_pubkey_alg_t pk_alg)
{
    dilithium_parameter_e dilithium_param = pk_alg_to_dilithium_id(pk_alg);
    pgp_curve_t           curve = pk_alg_to_curve_id(pk_alg);
    return exdsa_curve_pubkey_size(curve) + dilithium_pubkey_size(dilithium_param);
}

void
pgp_dilithium_exdsa_composite_public_key_t::parse_component_keys(
  std::vector<uint8_t> key_encoded)
{
    if (key_encoded.size() != encoded_size(pk_alg_)) {
        RNP_LOG("ML-DSA composite key format invalid: length mismatch");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    dilithium_parameter_e dilithium_param = pk_alg_to_dilithium_id(pk_alg_);
    pgp_curve_t           curve = pk_alg_to_curve_id(pk_alg_);
    size_t                split_at = exdsa_curve_pubkey_size(pk_alg_to_curve_id(pk_alg_));

    dilithium_key_ = pgp_dilithium_public_key_t(
      key_encoded.data() + split_at, key_encoded.size() - split_at, dilithium_param);
    exdsa_key_ = exdsa_public_key_t(key_encoded.data(), split_at, curve);

    is_initialized_ = true;
}

std::vector<uint8_t>
pgp_dilithium_exdsa_composite_public_key_t::get_encoded() const
{
    initialized_or_throw();
    std::vector<uint8_t> result;
    std::vector<uint8_t> exdsa_key_encoded = exdsa_key_.get_encoded();
    std::vector<uint8_t> dilithium_key_encoded = dilithium_key_.get_encoded();

    result.insert(result.end(), std::begin(exdsa_key_encoded), std::end(exdsa_key_encoded));
    result.insert(
      result.end(), std::begin(dilithium_key_encoded), std::end(dilithium_key_encoded));
    return result;
};

rnp_result_t
pgp_dilithium_exdsa_composite_public_key_t::verify(const pgp_dilithium_exdsa_signature_t *sig,
                                                   pgp_hash_alg_t hash_alg,
                                                   const uint8_t *hash,
                                                   size_t         hash_len) const
{
    initialized_or_throw();
    std::vector<uint8_t> dilithium_sig;
    std::vector<uint8_t> exdsa_sig;

    if (sig->sig.size() != sig->composite_signature_size(pk_alg_)) {
        RNP_LOG("invalid signature size for mldsa exdsa composite algorithm %d", pk_alg_);
        return RNP_ERROR_VERIFICATION_FAILED;
    }

    size_t split_at = exdsa_curve_signature_size(pk_alg_to_curve_id(pk_alg_));
    exdsa_sig = std::vector<uint8_t>(sig->sig.data(), sig->sig.data() + split_at);
    dilithium_sig =
      std::vector<uint8_t>(sig->sig.data() + split_at, sig->sig.data() + sig->sig.size());

    if (exdsa_key_.verify(exdsa_sig, hash, hash_len, hash_alg) != RNP_SUCCESS ||
        !dilithium_key_.verify_signature(
          hash, hash_len, dilithium_sig.data(), dilithium_sig.size())) {
        RNP_LOG("could not verify composite signature");
        return RNP_ERROR_VERIFICATION_FAILED;
    }

    return RNP_SUCCESS;
}

bool
pgp_dilithium_exdsa_composite_public_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized()) {
        return false;
    }
    return (exdsa_key_.is_valid(rng) && dilithium_key_.is_valid(rng));
}

bool
pgp_dilithium_exdsa_composite_private_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized()) {
        return false;
    }
    return (exdsa_key_->is_valid(rng) && dilithium_key_->is_valid(rng));
}

rnp_result_t
dilithium_exdsa_validate_key(rnp::RNG *rng, const pgp_dilithium_exdsa_key_t *key, bool secret)
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
