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

#include "exdsa_ecdhkem.h"
#include "ecdh.h"
#include "ed25519.h"
#include "ecdsa.h"
#include "ec.h"
#include "types.h"
#include "logging.h"
#include "string.h"
#include "utils.h"
#include <cassert>

ec_key_t::~ec_key_t()
{
}

ec_key_t::ec_key_t(pgp_curve_t curve) : curve_(curve)
{
}

ecdh_kem_public_key_t::ecdh_kem_public_key_t(uint8_t *   key_buf,
                                             size_t      key_buf_len,
                                             pgp_curve_t curve)
    : ec_key_t(curve), key_(std::vector<uint8_t>(key_buf, key_buf + key_buf_len))
{
}
ecdh_kem_public_key_t::ecdh_kem_public_key_t(std::vector<uint8_t> key, pgp_curve_t curve)
    : ec_key_t(curve), key_(key)
{
}

ecdh_kem_private_key_t::ecdh_kem_private_key_t(uint8_t *   key_buf,
                                               size_t      key_buf_len,
                                               pgp_curve_t curve)
    : ec_key_t(curve), key_(key_buf, key_buf + key_buf_len)
{
}
ecdh_kem_private_key_t::ecdh_kem_private_key_t(std::vector<uint8_t> key, pgp_curve_t curve)
    : ec_key_t(curve), key_(Botan::secure_vector<uint8_t>(key.begin(), key.end()))
{
}

Botan::ECDH_PrivateKey
ecdh_kem_private_key_t::botan_key_ecdh(rnp::RNG *rng) const
{
    assert(curve_ >= PGP_CURVE_NIST_P_256 && curve_ <= PGP_CURVE_P256K1);
    auto ec_desc = pgp::ec::Curve::get(curve_);
    return Botan::ECDH_PrivateKey(
      *(rng->obj()), Botan::EC_Group(ec_desc->botan_name), Botan::BigInt(key_));
}

Botan::ECDH_PublicKey
ecdh_kem_public_key_t::botan_key_ecdh(rnp::RNG *rng) const
{
    assert(curve_ >= PGP_CURVE_NIST_P_256 && curve_ <= PGP_CURVE_P256K1);

    auto            ec_desc = pgp::ec::Curve::get(curve_);
    Botan::EC_Group group(ec_desc->botan_name);
    const size_t    curve_order = ec_desc->bytes();
    Botan::BigInt   x(key_.data() + 1, curve_order);
    Botan::BigInt   y(key_.data() + 1 + curve_order, curve_order);
    return Botan::ECDH_PublicKey(group, group.point(x, y));
}

Botan::Curve25519_PrivateKey
ecdh_kem_private_key_t::botan_key_x25519() const
{
    assert(curve_ == PGP_CURVE_25519);
    return Botan::Curve25519_PrivateKey(key_);
}

Botan::Curve25519_PublicKey
ecdh_kem_public_key_t::botan_key_x25519() const
{
    assert(curve_ == PGP_CURVE_25519);
    return Botan::Curve25519_PublicKey(key_);
}

std::vector<uint8_t>
ecdh_kem_private_key_t::get_pubkey_encoded(rnp::RNG *rng) const
{
    if (curve_ == PGP_CURVE_25519) {
        Botan::X25519_PrivateKey botan_key = botan_key_x25519();
        return botan_key.public_value();
    } else {
        Botan::ECDH_PrivateKey botan_key = botan_key_ecdh(rng);
        return botan_key.public_value();
    }
}

rnp_result_t
ecdh_kem_public_key_t::encapsulate(rnp::RNG *            rng,
                                   std::vector<uint8_t> &ciphertext,
                                   std::vector<uint8_t> &symmetric_key) const
{
    if (curve_ == PGP_CURVE_25519) {
        Botan::Curve25519_PrivateKey eph_prv_key(*(rng->obj()));
        ciphertext = eph_prv_key.public_value();
        Botan::PK_Key_Agreement key_agreement(eph_prv_key, *(rng->obj()), "Raw");
        symmetric_key = Botan::unlock(key_agreement.derive_key(0, key_).bits_of());
    } else {
        auto curve_desc = pgp::ec::Curve::get(curve_);
        if (!curve_desc) {
            RNP_LOG("unknown curve");
            return RNP_ERROR_NOT_SUPPORTED;
        }

        Botan::EC_Group         domain(curve_desc->botan_name);
        Botan::ECDH_PrivateKey  eph_prv_key(*(rng->obj()), domain);
        Botan::PK_Key_Agreement key_agreement(eph_prv_key, *(rng->obj()), "Raw");
        ciphertext = eph_prv_key.public_value();
        symmetric_key = Botan::unlock(key_agreement.derive_key(0, key_).bits_of());
    }
    return RNP_SUCCESS;
}

rnp_result_t
ecdh_kem_private_key_t::decapsulate(rnp::RNG *                  rng,
                                    const std::vector<uint8_t> &ciphertext,
                                    std::vector<uint8_t> &      plaintext)
{
    if (curve_ == PGP_CURVE_25519) {
        Botan::Curve25519_PrivateKey priv_key = botan_key_x25519();
        Botan::PK_Key_Agreement      key_agreement(priv_key, *(rng->obj()), "Raw");
        plaintext = Botan::unlock(key_agreement.derive_key(0, ciphertext).bits_of());
    } else {
        Botan::ECDH_PrivateKey  priv_key = botan_key_ecdh(rng);
        Botan::PK_Key_Agreement key_agreement(priv_key, *(rng->obj()), "Raw");
        plaintext = Botan::unlock(key_agreement.derive_key(0, ciphertext).bits_of());
    }
    return RNP_SUCCESS;
}

rnp_result_t
ec_key_t::generate_ecdh_kem_key_pair(rnp::RNG *rng, ecdh_kem_key_t *out, pgp_curve_t curve)
{
    std::vector<uint8_t> pub, priv;
    rnp_result_t         result = ecdh_kem_gen_keypair_native(rng, priv, pub, curve);
    if (result != RNP_SUCCESS) {
        RNP_LOG("error when generating EC key pair");
        return result;
    }

    out->priv = ecdh_kem_private_key_t(priv, curve);
    out->pub = ecdh_kem_public_key_t(pub, curve);

    return RNP_SUCCESS;
}

exdsa_public_key_t::exdsa_public_key_t(uint8_t *key_buf, size_t key_buf_len, pgp_curve_t curve)
    : ec_key_t(curve), key_(key_buf, key_buf + key_buf_len)
{
}
exdsa_public_key_t::exdsa_public_key_t(std::vector<uint8_t> key, pgp_curve_t curve)
    : ec_key_t(curve), key_(key)
{
}

exdsa_private_key_t::exdsa_private_key_t(uint8_t *   key_buf,
                                         size_t      key_buf_len,
                                         pgp_curve_t curve)
    : ec_key_t(curve), key_(key_buf, key_buf + key_buf_len)
{
}
exdsa_private_key_t::exdsa_private_key_t(std::vector<uint8_t> key, pgp_curve_t curve)
    : ec_key_t(curve), key_(Botan::secure_vector<uint8_t>(key.begin(), key.end()))
{
}

rnp_result_t
ec_key_t::generate_exdsa_key_pair(rnp::RNG *rng, exdsa_key_t *out, pgp_curve_t curve)
{
    std::vector<uint8_t> pub, priv;
    rnp_result_t         result = exdsa_gen_keypair_native(rng, priv, pub, curve);
    if (result != RNP_SUCCESS) {
        RNP_LOG("error when generating EC key pair");
        return result;
    }

    out->priv = exdsa_private_key_t(priv, curve);
    out->pub = exdsa_public_key_t(pub, curve);

    return RNP_SUCCESS;
}

Botan::ECDSA_PrivateKey
exdsa_private_key_t::botan_key(rnp::RNG *rng) const
{
    auto                    ec_desc = pgp::ec::Curve::get(curve_);
    Botan::ECDSA_PrivateKey priv_key(
      *(rng->obj()), Botan::EC_Group(ec_desc->botan_name), Botan::BigInt(key_));
    return priv_key;
}

Botan::ECDSA_PublicKey
exdsa_public_key_t::botan_key() const
{
    // format: 04 | X | Y
    auto            ec_desc = pgp::ec::Curve::get(curve_);
    Botan::EC_Group group(ec_desc->botan_name);
    const size_t    curve_order = ec_desc->bytes();
    Botan::BigInt   x(key_.data() + 1, curve_order);
    Botan::BigInt   y(key_.data() + 1 + curve_order, curve_order);
    return Botan::ECDSA_PublicKey(group, group.point(x, y));
}

/* NOTE hash_alg unused for ed25519/x25519 curves */
rnp_result_t
exdsa_private_key_t::sign(rnp::RNG *            rng,
                          std::vector<uint8_t> &sig_out,
                          const uint8_t *       hash,
                          size_t                hash_len,
                          pgp_hash_alg_t        hash_alg) const
{
    if (curve_ == PGP_CURVE_ED25519) {
        return ed25519_sign_native(rng, sig_out, Botan::unlock(key_), hash, hash_len);
    } else {
        Botan::ECDSA_PrivateKey priv_key = botan_key(rng);
        auto                    signer =
          Botan::PK_Signer(priv_key, *(rng->obj()), pgp::ecdsa::padding_str_for(hash_alg));
        sig_out = signer.sign_message(hash, hash_len, *(rng->obj()));
    }
    return RNP_SUCCESS;
}

rnp_result_t
exdsa_public_key_t::verify(const std::vector<uint8_t> &sig,
                           const uint8_t *             hash,
                           size_t                      hash_len,
                           pgp_hash_alg_t              hash_alg) const
{
    if (curve_ == PGP_CURVE_ED25519) {
        return ed25519_verify_native(sig, key_, hash, hash_len);
    } else {
        Botan::ECDSA_PublicKey pub_key = botan_key();
        auto verifier = Botan::PK_Verifier(pub_key, pgp::ecdsa::padding_str_for(hash_alg));
        if (verifier.verify_message(hash, hash_len, sig.data(), sig.size())) {
            return RNP_SUCCESS;
        }
    }
    return RNP_ERROR_VERIFICATION_FAILED;
}

bool
exdsa_public_key_t::is_valid(rnp::RNG *rng) const
{
    if (curve_ == PGP_CURVE_ED25519) {
        Botan::Ed25519_PublicKey pub_key(key_);
        return pub_key.check_key(*(rng->obj()), false);
    } else {
        Botan::ECDSA_PublicKey pub_key = botan_key();
        return pub_key.check_key(*(rng->obj()), false);
    }
}

bool
exdsa_private_key_t::is_valid(rnp::RNG *rng) const
{
    if (curve_ == PGP_CURVE_ED25519) {
        Botan::Ed25519_PrivateKey priv_key(key_);
        return priv_key.check_key(*(rng->obj()), false);
    } else {
        auto priv_key = botan_key(rng);
        return priv_key.check_key(*(rng->obj()), false);
    }
}

bool
ecdh_kem_public_key_t::is_valid(rnp::RNG *rng) const
{
    if (curve_ == PGP_CURVE_25519) {
        auto pub_key = botan_key_x25519();
        return pub_key.check_key(*(rng->obj()), false);
    } else {
        auto pub_key = botan_key_ecdh(rng);
        return pub_key.check_key(*(rng->obj()), false);
    }
}

bool
ecdh_kem_private_key_t::is_valid(rnp::RNG *rng) const
{
    if (curve_ == PGP_CURVE_25519) {
        auto priv_key = botan_key_x25519();
        return priv_key.check_key(*(rng->obj()), false);
    } else {
        auto priv_key = botan_key_ecdh(rng);
        return priv_key.check_key(*(rng->obj()), false);
    }
}
