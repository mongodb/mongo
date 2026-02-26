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

#include "kyber_ecdh_composite.h"
#include "logging.h"
#include "types.h"
#include "ecdh_utils.h"
#include "kmac.hpp"
#include <botan/rfc3394.h>
#include <botan/symkey.h>
#include <cassert>

pgp_kyber_ecdh_composite_key_t::~pgp_kyber_ecdh_composite_key_t()
{
}

void
pgp_kyber_ecdh_composite_key_t::initialized_or_throw() const
{
    if (!is_initialized()) {
        RNP_LOG("Trying to use uninitialized kyber-ecdh key");
        throw rnp::rnp_exception(RNP_ERROR_GENERIC); /* TODO better return error */
    }
}

rnp_result_t
pgp_kyber_ecdh_composite_key_t::gen_keypair(rnp::RNG *            rng,
                                            pgp_kyber_ecdh_key_t *key,
                                            pgp_pubkey_alg_t      alg)
{
    rnp_result_t      res;
    pgp_curve_t       curve = pk_alg_to_curve_id(alg);
    kyber_parameter_e kyber_id = pk_alg_to_kyber_id(alg);

    ecdh_kem_key_t ecdh_key_pair;

    res = ec_key_t::generate_ecdh_kem_key_pair(rng, &ecdh_key_pair, curve);
    if (res != RNP_SUCCESS) {
        RNP_LOG("generating kyber ecdh composite key failed when generating ecdh key");
        return res;
    }

    auto kyber_key_pair = kyber_generate_keypair(rng, kyber_id);

    key->priv = pgp_kyber_ecdh_composite_private_key_t(
      ecdh_key_pair.priv.get_encoded(), kyber_key_pair.second.get_encoded(), alg);
    key->pub = pgp_kyber_ecdh_composite_public_key_t(
      ecdh_key_pair.pub.get_encoded(), kyber_key_pair.first.get_encoded(), alg);

    return RNP_SUCCESS;
}

size_t
pgp_kyber_ecdh_composite_key_t::ecdh_curve_privkey_size(pgp_curve_t curve)
{
    switch (curve) {
    case PGP_CURVE_25519:
        return 32;
    /* TODO */
    // case PGP_CURVE_X448:
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
pgp_kyber_ecdh_composite_key_t::ecdh_curve_pubkey_size(pgp_curve_t curve)
{
    switch (curve) {
    case PGP_CURVE_25519:
        return 32;
    /* TODO */
    //  case PGP_CURVE_X448:
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
pgp_kyber_ecdh_composite_key_t::ecdh_curve_ephemeral_size(pgp_curve_t curve)
{
    switch (curve) {
    case PGP_CURVE_25519:
        return 32;
    /* TODO */
    //  case PGP_CURVE_X448:
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
pgp_kyber_ecdh_composite_key_t::ecdh_curve_keyshare_size(pgp_curve_t curve)
{
    switch (curve) {
    case PGP_CURVE_25519:
        return 32;
    /* TODO */
    //  case PGP_CURVE_X448:
    //    return 56;
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

kyber_parameter_e
pgp_kyber_ecdh_composite_key_t::pk_alg_to_kyber_id(pgp_pubkey_alg_t pk_alg)
{
    switch (pk_alg) {
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        return kyber_768;
    case PGP_PKA_KYBER1024_BP384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        return kyber_1024;
    default:
        RNP_LOG("invalid PK alg given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

pgp_curve_t
pgp_kyber_ecdh_composite_key_t::pk_alg_to_curve_id(pgp_pubkey_alg_t pk_alg)
{
    switch (pk_alg) {
    case PGP_PKA_KYBER768_X25519:
        return PGP_CURVE_25519;
    case PGP_PKA_KYBER768_P256:
        return PGP_CURVE_NIST_P_256;
    case PGP_PKA_KYBER768_BP256:
        return PGP_CURVE_BP256;
    case PGP_PKA_KYBER1024_BP384:
        return PGP_CURVE_BP384;
    case PGP_PKA_KYBER1024_P384:
        return PGP_CURVE_NIST_P_384;
    /*case PGP_PKA_KYBER1024_X448:
      return ... NOT_IMPLEMENTED*/
    default:
        RNP_LOG("invalid PK alg given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

pgp_kyber_ecdh_composite_private_key_t::pgp_kyber_ecdh_composite_private_key_t(
  const uint8_t *key_encoded, size_t key_encoded_len, pgp_pubkey_alg_t pk_alg)
    : pk_alg_(pk_alg)
{
    parse_component_keys(std::vector<uint8_t>(key_encoded, key_encoded + key_encoded_len));
}

pgp_kyber_ecdh_composite_private_key_t::pgp_kyber_ecdh_composite_private_key_t(
  std::vector<uint8_t> const &key_encoded, pgp_pubkey_alg_t pk_alg)
    : pk_alg_(pk_alg)
{
    parse_component_keys(key_encoded);
}

pgp_kyber_ecdh_composite_private_key_t::pgp_kyber_ecdh_composite_private_key_t(
  std::vector<uint8_t> const &ecdh_key_encoded,
  std::vector<uint8_t> const &kyber_key_encoded,
  pgp_pubkey_alg_t            pk_alg)
    : pk_alg_(pk_alg)
{
    if (ecdh_curve_privkey_size(pk_alg_to_curve_id(pk_alg)) != ecdh_key_encoded.size() ||
        kyber_privkey_size(pk_alg_to_kyber_id(pk_alg)) != kyber_key_encoded.size()) {
        RNP_LOG("ecdh or kyber key length mismatch");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    kyber_key_ = std::make_unique<pgp_kyber_private_key_t>(
      pgp_kyber_private_key_t(kyber_key_encoded, pk_alg_to_kyber_id(pk_alg)));
    ecdh_key_ = std::make_unique<ecdh_kem_private_key_t>(
      ecdh_kem_private_key_t(ecdh_key_encoded, pk_alg_to_curve_id(pk_alg)));

    is_initialized_ = true;
}

/* copy assignment operator is used on key materials struct and thus needs to be defined for
 * this class as well */
pgp_kyber_ecdh_composite_private_key_t &
pgp_kyber_ecdh_composite_private_key_t::operator=(
  const pgp_kyber_ecdh_composite_private_key_t &other)
{
    pgp_kyber_ecdh_composite_key_t::operator=(other);
    pk_alg_ = other.pk_alg_;
    if (other.is_initialized() && other.kyber_key_) {
        kyber_key_ = std::make_unique<pgp_kyber_private_key_t>(
          pgp_kyber_private_key_t(other.kyber_key_->get_encoded(), other.kyber_key_->param()));
    }
    if (other.is_initialized() && other.ecdh_key_) {
        ecdh_key_ = std::make_unique<ecdh_kem_private_key_t>(ecdh_kem_private_key_t(
          other.ecdh_key_->get_encoded(), other.ecdh_key_->get_curve()));
    }

    return *this;
}

pgp_kyber_ecdh_composite_private_key_t::pgp_kyber_ecdh_composite_private_key_t(
  const pgp_kyber_ecdh_composite_private_key_t &other)
{
    *this = other;
}

size_t
pgp_kyber_ecdh_composite_private_key_t::encoded_size(pgp_pubkey_alg_t pk_alg)
{
    kyber_parameter_e kyber_param = pk_alg_to_kyber_id(pk_alg);
    pgp_curve_t       curve = pk_alg_to_curve_id(pk_alg);
    return ecdh_curve_privkey_size(curve) + kyber_privkey_size(kyber_param);
}

void
pgp_kyber_ecdh_composite_private_key_t::parse_component_keys(std::vector<uint8_t> key_encoded)
{
    if (key_encoded.size() != encoded_size(pk_alg_)) {
        RNP_LOG("ML-KEM composite key format invalid: length mismatch");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    kyber_parameter_e kyber_param = pk_alg_to_kyber_id(pk_alg_);
    pgp_curve_t       ecdh_curve = pk_alg_to_curve_id(pk_alg_);
    size_t            split_at = ecdh_curve_privkey_size(pk_alg_to_curve_id(pk_alg_));

    kyber_key_ = std::make_unique<pgp_kyber_private_key_t>(pgp_kyber_private_key_t(
      key_encoded.data() + split_at, key_encoded.size() - split_at, kyber_param));
    ecdh_key_ = std::make_unique<ecdh_kem_private_key_t>(
      ecdh_kem_private_key_t(key_encoded.data(), split_at, ecdh_curve));

    is_initialized_ = true;
}

namespace {
std::vector<uint8_t>
hashed_ecc_keyshare(const std::vector<uint8_t> &key_share,
                    const std::vector<uint8_t> &ciphertext,
                    const std::vector<uint8_t> &ecc_pubkey,
                    pgp_pubkey_alg_t            alg_id)
{
    /* SHA3-256(X || eccCipherText) or SHA3-512(X || eccCipherText) depending on algorithm */

    std::vector<uint8_t> digest;
    pgp_hash_alg_t       hash_alg;

    switch (alg_id) {
    case PGP_PKA_KYBER768_X25519:
    case PGP_PKA_KYBER768_BP256:
    case PGP_PKA_KYBER768_P256:
        hash_alg = PGP_HASH_SHA3_256;
        break;
    // case PGP_PKA_KYBER1024_X448:
    case PGP_PKA_KYBER1024_P384:
    case PGP_PKA_KYBER1024_BP384:
        hash_alg = PGP_HASH_SHA3_512;
        break;
    default:
        RNP_LOG("key combiner does not support this algorithm");
        throw rnp::rnp_exception(RNP_ERROR_BAD_STATE);
    }

    auto hash = rnp::Hash::create(hash_alg);
    hash->add(key_share);
    hash->add(ciphertext);
    hash->add(ecc_pubkey);

    digest.resize(rnp::Hash::size(hash_alg));
    hash->finish(digest.data());

    return digest;
}
} // namespace

rnp_result_t
pgp_kyber_ecdh_composite_private_key_t::decrypt(rnp::RNG *                        rng,
                                                uint8_t *                         out,
                                                size_t *                          out_len,
                                                const pgp_kyber_ecdh_encrypted_t *enc) const
{
    initialized_or_throw();
    rnp_result_t         res;
    std::vector<uint8_t> ecdh_keyshare;
    std::vector<uint8_t> hashed_ecdh_keyshare;
    std::vector<uint8_t> kyber_keyshare;

    if (((enc->wrapped_sesskey.size() % 8) != 0) || (enc->wrapped_sesskey.size() < 16)) {
        RNP_LOG("invalid wrapped AES key length (size is a multiple of 8 octets with 8 octets "
                "integrity check)");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // Compute (eccKeyShare) := eccKem.decap(eccCipherText, eccPrivateKey)
    pgp_curve_t          curve = pk_alg_to_curve_id(pk_alg_);
    std::vector<uint8_t> ecdh_encapsulated_keyshare = std::vector<uint8_t>(
      enc->composite_ciphertext.data(),
      enc->composite_ciphertext.data() + ecdh_curve_ephemeral_size(curve));
    res = ecdh_key_->decapsulate(rng, ecdh_encapsulated_keyshare, ecdh_keyshare);
    if (res) {
        RNP_LOG("error when decrypting kyber-ecdh encrypted session key");
        return res;
    }
    hashed_ecdh_keyshare = hashed_ecc_keyshare(
      ecdh_keyshare, ecdh_encapsulated_keyshare, ecdh_key_->get_pubkey_encoded(rng), pk_alg_);

    // Compute (kyberKeyShare) := kyberKem.decap(kyberCipherText, kyberPrivateKey)
    std::vector<uint8_t> kyber_encapsulated_keyshare = std::vector<uint8_t>(
      enc->composite_ciphertext.begin() + ecdh_curve_ephemeral_size(curve),
      enc->composite_ciphertext.end());
    kyber_keyshare = kyber_key_->decapsulate(
      rng, kyber_encapsulated_keyshare.data(), kyber_encapsulated_keyshare.size());
    if (res) {
        RNP_LOG("error when decrypting kyber-ecdh encrypted session key");
        return res;
    }

    // Compute KEK := multiKeyCombine(eccKeyShare, kyberKeyShare, fixedInfo) as defined in
    // Section 4.2.2
    std::vector<uint8_t> kek_vec;
    auto                 kmac = rnp::KMAC256::create();
    kmac->compute(hashed_ecdh_keyshare,
                  ecdh_encapsulated_keyshare,
                  kyber_keyshare,
                  kyber_encapsulated_keyshare,
                  pk_alg(),
                  kek_vec);
    Botan::SymmetricKey kek(kek_vec);

    // Compute sessionKey := AESKeyUnwrap(KEK, C) with AES-256 as per [RFC3394], aborting if
    // the 64 bit integrity check fails
    Botan::secure_vector<uint8_t> tmp_out;
    try {
        tmp_out =
          Botan::rfc3394_keyunwrap(Botan::secure_vector<uint8_t>(enc->wrapped_sesskey.begin(),
                                                                 enc->wrapped_sesskey.end()),
                                   kek);
    } catch (const std::exception &e) {
        RNP_LOG("Keyunwrap failed: %s", e.what());
        return RNP_ERROR_DECRYPT_FAILED;
    }

    if (*out_len < tmp_out.size()) {
        RNP_LOG("buffer for decryption result too small");
        return RNP_ERROR_DECRYPT_FAILED;
    }
    *out_len = tmp_out.size();
    memcpy(out, tmp_out.data(), *out_len);

    return RNP_SUCCESS;
}

void
pgp_kyber_ecdh_composite_private_key_t::secure_clear()
{
    // private key buffer is stored in a secure_vector and will be securely erased by the
    // destructor.
    kyber_key_.reset();
    ecdh_key_.reset();
    is_initialized_ = false;
}

std::vector<uint8_t>
pgp_kyber_ecdh_composite_private_key_t::get_encoded() const
{
    initialized_or_throw();
    std::vector<uint8_t> result;
    std::vector<uint8_t> ecdh_key_encoded = ecdh_key_->get_encoded();
    std::vector<uint8_t> kyber_key_encoded = kyber_key_->get_encoded();

    result.insert(result.end(), std::begin(ecdh_key_encoded), std::end(ecdh_key_encoded));
    result.insert(result.end(), std::begin(kyber_key_encoded), std::end(kyber_key_encoded));
    return result;
};

pgp_kyber_ecdh_composite_public_key_t::pgp_kyber_ecdh_composite_public_key_t(
  const uint8_t *key_encoded, size_t key_encoded_len, pgp_pubkey_alg_t pk_alg)
    : pk_alg_(pk_alg)
{
    parse_component_keys(std::vector<uint8_t>(key_encoded, key_encoded + key_encoded_len));
}

pgp_kyber_ecdh_composite_public_key_t::pgp_kyber_ecdh_composite_public_key_t(
  std::vector<uint8_t> const &key_encoded, pgp_pubkey_alg_t pk_alg)
    : pk_alg_(pk_alg)
{
    parse_component_keys(key_encoded);
}

pgp_kyber_ecdh_composite_public_key_t::pgp_kyber_ecdh_composite_public_key_t(
  std::vector<uint8_t> const &ecdh_key_encoded,
  std::vector<uint8_t> const &kyber_key_encoded,
  pgp_pubkey_alg_t            pk_alg)
    : pk_alg_(pk_alg), kyber_key_(kyber_key_encoded, pk_alg_to_kyber_id(pk_alg)),
      ecdh_key_(ecdh_key_encoded, pk_alg_to_curve_id(pk_alg))
{
    if (ecdh_curve_pubkey_size(pk_alg_to_curve_id(pk_alg)) != ecdh_key_encoded.size() ||
        kyber_pubkey_size(pk_alg_to_kyber_id(pk_alg)) != kyber_key_encoded.size()) {
        RNP_LOG("ecdh or kyber key length mismatch");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
    is_initialized_ = true;
}

size_t
pgp_kyber_ecdh_composite_public_key_t::encoded_size(pgp_pubkey_alg_t pk_alg)
{
    kyber_parameter_e kyber_param = pk_alg_to_kyber_id(pk_alg);
    pgp_curve_t       curve = pk_alg_to_curve_id(pk_alg);
    return ecdh_curve_pubkey_size(curve) + kyber_pubkey_size(kyber_param);
}

void
pgp_kyber_ecdh_composite_public_key_t::parse_component_keys(std::vector<uint8_t> key_encoded)
{
    if (key_encoded.size() != encoded_size(pk_alg_)) {
        RNP_LOG("ML-KEM composite key format invalid: length mismatch");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    kyber_parameter_e kyber_param = pk_alg_to_kyber_id(pk_alg_);
    pgp_curve_t       ecdh_curve = pk_alg_to_curve_id(pk_alg_);
    size_t            split_at = ecdh_curve_pubkey_size(pk_alg_to_curve_id(pk_alg_));

    kyber_key_ = pgp_kyber_public_key_t(
      key_encoded.data() + split_at, key_encoded.size() - split_at, kyber_param);
    ecdh_key_ = ecdh_kem_public_key_t(key_encoded.data(), split_at, ecdh_curve);

    is_initialized_ = true;
}

rnp_result_t
pgp_kyber_ecdh_composite_public_key_t::encrypt(rnp::RNG *                  rng,
                                               pgp_kyber_ecdh_encrypted_t *out,
                                               const uint8_t *             session_key,
                                               size_t session_key_len) const
{
    initialized_or_throw();

    rnp_result_t         res;
    std::vector<uint8_t> ecdh_ciphertext;
    std::vector<uint8_t> ecdh_symmetric_key;
    std::vector<uint8_t> ecdh_hashed_symmetric_key;

    if ((session_key_len % 8) != 0) {
        RNP_LOG("AES key wrap requires a multiple of 8 octets as input key");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // Compute (eccCipherText, eccKeyShare) := eccKem.encap(eccPublicKey)
    res = ecdh_key_.encapsulate(rng, ecdh_ciphertext, ecdh_symmetric_key);
    if (res) {
        RNP_LOG("error when encapsulating with ECDH");
        return res;
    }
    ecdh_hashed_symmetric_key = hashed_ecc_keyshare(
      ecdh_symmetric_key, ecdh_ciphertext, ecdh_key_.get_encoded(), pk_alg_);

    // Compute (kyberCipherText, kyberKeyShare) := kyberKem.encap(kyberPublicKey)
    kyber_encap_result_t kyber_encap = kyber_key_.encapsulate(rng);

    // Compute KEK := multiKeyCombine(eccKeyShare, kyberKeyShare, fixedInfo) as defined in
    // Section 4.2.2
    std::vector<uint8_t> kek_vec;
    auto                 kmac = rnp::KMAC256::create();
    kmac->compute(ecdh_hashed_symmetric_key,
                  ecdh_ciphertext,
                  kyber_encap.symmetric_key,
                  kyber_encap.ciphertext,
                  pk_alg(),
                  kek_vec);
    Botan::SymmetricKey kek(kek_vec);

    // Compute C := AESKeyWrap(KEK, sessionKey) with AES-256 as per [RFC3394] that includes a
    // 64 bit integrity check
    try {
        out->wrapped_sesskey = Botan::unlock(Botan::rfc3394_keywrap(
          Botan::secure_vector<uint8_t>(session_key, session_key + session_key_len), kek));
    } catch (const std::exception &e) {
        RNP_LOG("Keywrap failed: %s", e.what());
        return RNP_ERROR_ENCRYPT_FAILED;
    }

    out->composite_ciphertext.assign(ecdh_ciphertext.begin(), ecdh_ciphertext.end());
    out->composite_ciphertext.insert(out->composite_ciphertext.end(),
                                     kyber_encap.ciphertext.begin(),
                                     kyber_encap.ciphertext.end());
    return RNP_SUCCESS;
}

std::vector<uint8_t>
pgp_kyber_ecdh_composite_public_key_t::get_encoded() const
{
    initialized_or_throw();
    std::vector<uint8_t> result;
    std::vector<uint8_t> ecdh_key_encoded = ecdh_key_.get_encoded();
    std::vector<uint8_t> kyber_key_encoded = kyber_key_.get_encoded();

    result.insert(result.end(), std::begin(ecdh_key_encoded), std::end(ecdh_key_encoded));
    result.insert(result.end(), std::begin(kyber_key_encoded), std::end(kyber_key_encoded));
    return result;
};

bool
pgp_kyber_ecdh_composite_public_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized()) {
        return false;
    }
    return (ecdh_key_.is_valid(rng) && kyber_key_.is_valid(rng));
}

bool
pgp_kyber_ecdh_composite_private_key_t::is_valid(rnp::RNG *rng) const
{
    if (!is_initialized()) {
        return false;
    }
    return (ecdh_key_->is_valid(rng) && kyber_key_->is_valid(rng));
}

rnp_result_t
kyber_ecdh_validate_key(rnp::RNG *rng, const pgp_kyber_ecdh_key_t *key, bool secret)
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
