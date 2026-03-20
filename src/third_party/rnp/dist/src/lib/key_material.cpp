/*
 * Copyright (c) 2024 [Ribose Inc](https://www.ribose.com).
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

#include "key_material.hpp"
#include "librepgp/stream-packet.h"
#include "logging.h"
#include "utils.h"
#include "config.h"
#include <cassert>

namespace {
void
grip_hash_mpi(rnp::Hash &hash, const pgp::mpi &val, const char name, bool lzero = true)
{
    size_t len = val.size();
    size_t idx = 0;
    for (idx = 0; (idx < len) && !val[idx]; idx++)
        ;

    if (name) {
        size_t hlen = idx >= len ? 0 : len - idx;
        if ((len > idx) && lzero && (val[idx] & 0x80)) {
            hlen++;
        }

        char buf[26] = {0};
        snprintf(buf, sizeof(buf), "(1:%c%zu:", name, hlen);
        hash.add(buf, strlen(buf));
    }

    if (idx < len) {
        /* gcrypt prepends mpis with zero if higher bit is set */
        if (lzero && (val[idx] & 0x80)) {
            uint8_t zero = 0;
            hash.add(&zero, 1);
        }
        hash.add(val.data() + idx, len - idx);
    }
    if (name) {
        hash.add(")", 1);
    }
}

void
grip_hash_ecc_hex(rnp::Hash &hash, const char *hex, char name)
{
    auto bin = rnp::hex_to_bin(hex);
    if (bin.empty()) {
        RNP_LOG("wrong hex mpi");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    pgp::mpi mpi;
    mpi.assign(bin.data(), bin.size());

    /* libgcrypt doesn't add leading zero when hashes ecc mpis */
    return grip_hash_mpi(hash, mpi, name, false);
}

void
grip_hash_ec(rnp::Hash &hash, const pgp::ec::Key &key)
{
    auto desc = pgp::ec::Curve::get(key.curve);
    if (!desc) {
        RNP_LOG("unknown curve %d", (int) key.curve);
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    /* build uncompressed point from gx and gy */
    auto gxbin = rnp::hex_to_bin(desc->gx);
    auto gybin = rnp::hex_to_bin(desc->gy);
    assert(!gxbin.empty());
    assert(!gybin.empty());
    pgp::mpi g;
    g.resize(1 + gxbin.size() + gybin.size());
    g[0] = 0x04;
    memcpy(g.data() + 1, gxbin.data(), gxbin.size());
    memcpy(g.data() + 1 + gxbin.size(), gybin.data(), gybin.size());

    /* p, a, b, g, n, q */
    grip_hash_ecc_hex(hash, desc->p, 'p');
    grip_hash_ecc_hex(hash, desc->a, 'a');
    grip_hash_ecc_hex(hash, desc->b, 'b');
    grip_hash_mpi(hash, g, 'g', false);
    grip_hash_ecc_hex(hash, desc->n, 'n');

    if ((key.curve == PGP_CURVE_ED25519) || (key.curve == PGP_CURVE_25519)) {
        if (g.size() < 1) {
            RNP_LOG("wrong 25519 p");
            throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
        }
        g.resize(key.p.size() - 1);
        memcpy(g.data(), key.p.data() + 1, g.size());
        grip_hash_mpi(hash, g, 'q', false);
    } else {
        grip_hash_mpi(hash, key.p, 'q', false);
    }
}
} // namespace

namespace pgp {

KeyParams::~KeyParams()
{
}

std::unique_ptr<KeyParams>
KeyParams::create(pgp_pubkey_alg_t alg)
{
    switch (alg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY:
        return std::unique_ptr<KeyParams>(new RSAKeyParams());
    case PGP_PKA_ECDSA:
        return std::unique_ptr<KeyParams>(new ECDSAKeyParams());
    case PGP_PKA_ECDH:
        return std::unique_ptr<KeyParams>(new ECCKeyParams());
    case PGP_PKA_EDDSA:
        return std::unique_ptr<KeyParams>(new ECCKeyParams(PGP_CURVE_ED25519));
    case PGP_PKA_SM2:
        return std::unique_ptr<KeyParams>(new ECCKeyParams(PGP_CURVE_SM2_P_256));
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
        return std::unique_ptr<KeyParams>(new ECCKeyParams(PGP_CURVE_ED25519));
    case PGP_PKA_X25519:
        return std::unique_ptr<KeyParams>(new ECCKeyParams(PGP_CURVE_25519));
#endif
    case PGP_PKA_DSA:
        return std::unique_ptr<KeyParams>(new DSAKeyParams());
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN:
        return std::unique_ptr<KeyParams>(new EGKeyParams());
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384:
        return std::unique_ptr<KeyParams>(new MlkemEcdhKeyParams(alg));
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448 with FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        return std::unique_ptr<KeyParams>(new DilithiumEccKeyParams(alg));
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        return std::unique_ptr<KeyParams>(new SlhdsaKeyParams());
#endif
    default:
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

void
DSAKeyParams::check_defaults() noexcept
{
    if (!qbits_) {
        qbits_ = dsa::Key::choose_qsize(bits());
    }
}

pgp_hash_alg_t
DSAKeyParams::min_hash() const noexcept
{
    return dsa::Key::get_min_hash(qbits_);
}

size_t
ECCKeyParams::bits() const noexcept
{
    auto curve = ec::Curve::get(curve_);
    return curve ? curve->bitlen : 0;
}

pgp_hash_alg_t
ECDSAKeyParams::min_hash() const noexcept
{
    return ecdsa::get_min_hash(curve());
}

#if defined(ENABLE_PQC)
size_t
MlkemEcdhKeyParams::bits() const noexcept
{
    return pgp_kyber_ecdh_composite_public_key_t::encoded_size(alg_) * 8;
}

size_t
DilithiumEccKeyParams::bits() const noexcept
{
    return pgp_dilithium_exdsa_composite_public_key_t::encoded_size(alg_) * 8;
}

size_t
SlhdsaKeyParams::bits() const noexcept
{
    return sphincsplus_pubkey_size(param_) * 8;
}
#endif

KeyMaterial::~KeyMaterial()
{
}

pgp_pubkey_alg_t
KeyMaterial::alg() const noexcept
{
    return alg_;
}

bool
KeyMaterial::secret() const noexcept
{
    return secret_;
}

bool
KeyMaterial::valid() const
{
    return validity_.validated && validity_.valid;
}

void
KeyMaterial::validate(rnp::SecurityContext &ctx, bool reset)
{
    if (!reset && validity_.validated) {
        return;
    }
    validity_.reset();
#ifdef FUZZERS_ENABLED
    /* do not timeout on large keys during fuzzing */
    validity_.valid = true;
#else
    validity_.valid = validate_material(ctx, reset);
#endif
    validity_.validated = true;
}

const pgp_validity_t &
KeyMaterial::validity() const noexcept
{
    return validity_;
}

void
KeyMaterial::set_validity(const pgp_validity_t &val)
{
    validity_ = val;
}

void
KeyMaterial::reset_validity()
{
    validity_.reset();
}

void
KeyMaterial::clear_secret() noexcept
{
    secret_ = false;
}

bool
KeyMaterial::finish_generate()
{
    validity_.mark_valid();
    secret_ = true;
    return true;
}

bool
KeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    RNP_LOG("key generation not implemented for PK alg: %d", alg_);
    return false;
}

rnp_result_t
KeyMaterial::encrypt(rnp::SecurityContext &   ctx,
                     EncMaterial &            out,
                     const rnp::secure_bytes &data) const
{
    return RNP_ERROR_NOT_SUPPORTED;
}

rnp_result_t
KeyMaterial::decrypt(rnp::SecurityContext &ctx,
                     rnp::secure_bytes &   out,
                     const EncMaterial &   in) const
{
    return RNP_ERROR_NOT_SUPPORTED;
}

rnp_result_t
KeyMaterial::verify(const rnp::SecurityContext &ctx,
                    const SigMaterial &         sig,
                    const rnp::secure_bytes &   hash) const
{
    return RNP_ERROR_NOT_SUPPORTED;
}

rnp_result_t
KeyMaterial::sign(rnp::SecurityContext &   ctx,
                  SigMaterial &            sig,
                  const rnp::secure_bytes &hash) const
{
    return RNP_ERROR_NOT_SUPPORTED;
}

pgp_hash_alg_t
KeyMaterial::adjust_hash(pgp_hash_alg_t hash) const
{
    return hash;
}

bool
KeyMaterial::sig_hash_allowed(pgp_hash_alg_t hash) const
{
    return true;
}

pgp_curve_t
KeyMaterial::curve() const noexcept
{
    return PGP_CURVE_UNKNOWN;
}

KeyGrip
KeyMaterial::grip() const
{
    auto hash = rnp::Hash::create(PGP_HASH_SHA1);
    grip_update(*hash);
    KeyGrip res{};
    hash->finish(res.data());
    return res;
}

std::unique_ptr<KeyMaterial>
KeyMaterial::create(pgp_pubkey_alg_t alg)
{
    switch (alg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
    case PGP_PKA_RSA_SIGN_ONLY:
        return std::unique_ptr<KeyMaterial>(new RSAKeyMaterial(alg));
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN:
        return std::unique_ptr<KeyMaterial>(new EGKeyMaterial(alg));
    case PGP_PKA_DSA:
        return std::unique_ptr<KeyMaterial>(new DSAKeyMaterial());
    case PGP_PKA_ECDH:
        return std::unique_ptr<KeyMaterial>(new ECDHKeyMaterial());
    case PGP_PKA_ECDSA:
        return std::unique_ptr<KeyMaterial>(new ECDSAKeyMaterial());
    case PGP_PKA_EDDSA:
        return std::unique_ptr<KeyMaterial>(new EDDSAKeyMaterial());
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519:
        return std::unique_ptr<KeyMaterial>(new Ed25519KeyMaterial());
    case PGP_PKA_X25519:
        return std::unique_ptr<KeyMaterial>(new X25519KeyMaterial());
#endif
    case PGP_PKA_SM2:
        return std::unique_ptr<KeyMaterial>(new SM2KeyMaterial());
#if defined(ENABLE_PQC)
    case PGP_PKA_KYBER768_X25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_KYBER1024_X448
    case PGP_PKA_KYBER768_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER768_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_KYBER1024_BP384:
        return std::unique_ptr<KeyMaterial>(new MlkemEcdhKeyMaterial(alg));
    case PGP_PKA_DILITHIUM3_ED25519:
        FALLTHROUGH_STATEMENT;
    // TODO: Add case for PGP_PKA_DILITHIUM5_ED448
    case PGP_PKA_DILITHIUM3_P256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_P384:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM3_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_DILITHIUM5_BP384:
        return std::unique_ptr<KeyMaterial>(new DilithiumEccKeyMaterial(alg));
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        return std::unique_ptr<KeyMaterial>(new SlhdsaKeyMaterial(alg));
#endif
    default:
        return nullptr;
    }
}

std::unique_ptr<KeyMaterial>
KeyMaterial::create(pgp_pubkey_alg_t alg, const rsa::Key &key)
{
    return std::unique_ptr<KeyMaterial>(new RSAKeyMaterial(alg, key));
}

std::unique_ptr<KeyMaterial>
KeyMaterial::create(const dsa::Key &key)
{
    return std::unique_ptr<KeyMaterial>(new DSAKeyMaterial(key));
}

std::unique_ptr<KeyMaterial>
KeyMaterial::create(pgp_pubkey_alg_t alg, const eg::Key &key)
{
    return std::unique_ptr<KeyMaterial>(new EGKeyMaterial(alg, key));
}

std::unique_ptr<KeyMaterial>
KeyMaterial::create(pgp_pubkey_alg_t alg, const ec::Key &key)
{
    switch (alg) {
    case PGP_PKA_ECDSA:
        return std::unique_ptr<KeyMaterial>(new ECDSAKeyMaterial(key));
    case PGP_PKA_ECDH:
        return std::unique_ptr<KeyMaterial>(new ECDHKeyMaterial(key));
    case PGP_PKA_EDDSA:
        return std::unique_ptr<KeyMaterial>(new EDDSAKeyMaterial(key));
    case PGP_PKA_SM2:
        return std::unique_ptr<KeyMaterial>(new SM2KeyMaterial(key));
    default:
        throw std::invalid_argument("Invalid EC algorithm.");
    }
}

std::unique_ptr<KeyMaterial>
RSAKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new RSAKeyMaterial(*this));
}

void
RSAKeyMaterial::grip_update(rnp::Hash &hash) const
{
    /* keygrip is subjectKeyHash from pkcs#15 for RSA. */
    grip_hash_mpi(hash, key_.n, '\0');
}

bool
RSAKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return !key_.validate(ctx.rng, secret_);
}

void
RSAKeyMaterial::clear_secret() noexcept
{
    key_.clear_secret();
    KeyMaterial::clear_secret();
}

bool
RSAKeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    return pkt.get(key_.n) && pkt.get(key_.e);
}

bool
RSAKeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    if (!pkt.get(key_.d) || !pkt.get(key_.p) || !pkt.get(key_.q) || !pkt.get(key_.u)) {
        RNP_LOG("failed to parse rsa secret key data");
        return false;
    }
    secret_ = true;
    return true;
}

void
RSAKeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.n);
    pkt.add(key_.e);
}

void
RSAKeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.d);
    pkt.add(key_.p);
    pkt.add(key_.q);
    pkt.add(key_.u);
}

bool
RSAKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    /* We do not generate PGP_PKA_RSA_ENCRYPT_ONLY or PGP_PKA_RSA_SIGN_ONLY keys */
    if (alg_ != PGP_PKA_RSA) {
        RNP_LOG("Unsupported algorithm for key generation: %d", alg_);
        return false;
    }
    if (key_.generate(ctx.rng, params.bits())) {
        RNP_LOG("failed to generate RSA key");
        return false;
    }
    return finish_generate();
}

rnp_result_t
RSAKeyMaterial::encrypt(rnp::SecurityContext &   ctx,
                        EncMaterial &            out,
                        const rnp::secure_bytes &data) const
{
    auto rsa = dynamic_cast<RSAEncMaterial *>(&out);
    if (!rsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.encrypt_pkcs1(ctx.rng, rsa->enc, data);
}

rnp_result_t
RSAKeyMaterial::decrypt(rnp::SecurityContext &ctx,
                        rnp::secure_bytes &   out,
                        const EncMaterial &   in) const
{
    auto rsa = dynamic_cast<const RSAEncMaterial *>(&in);
    if (!rsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.decrypt_pkcs1(ctx.rng, out, rsa->enc);
}

rnp_result_t
RSAKeyMaterial::verify(const rnp::SecurityContext &ctx,
                       const SigMaterial &         sig,
                       const rnp::secure_bytes &   hash) const
{
    if (alg() == PGP_PKA_RSA_ENCRYPT_ONLY) {
        RNP_LOG("RSA encrypt-only signature considered as invalid.");
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    auto rsa = dynamic_cast<const RSASigMaterial *>(&sig);
    if (!rsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.verify_pkcs1(rsa->sig, rsa->halg, hash);
}

rnp_result_t
RSAKeyMaterial::sign(rnp::SecurityContext &   ctx,
                     SigMaterial &            sig,
                     const rnp::secure_bytes &hash) const
{
    auto rsa = dynamic_cast<RSASigMaterial *>(&sig);
    if (!rsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.sign_pkcs1(ctx.rng, rsa->sig, rsa->halg, hash);
}

void
RSAKeyMaterial::set_secret(const mpi &d, const mpi &p, const mpi &q, const mpi &u)
{
    key_.d = d;
    key_.p = p;
    key_.q = q;
    key_.u = u;
    secret_ = true;
}

size_t
RSAKeyMaterial::bits() const noexcept
{
    return 8 * key_.n.size();
}

const mpi &
RSAKeyMaterial::n() const noexcept
{
    return key_.n;
}

const mpi &
RSAKeyMaterial::e() const noexcept
{
    return key_.e;
}

const mpi &
RSAKeyMaterial::d() const noexcept
{
    return key_.d;
}

const mpi &
RSAKeyMaterial::p() const noexcept
{
    return key_.p;
}

const mpi &
RSAKeyMaterial::q() const noexcept
{
    return key_.q;
}

const mpi &
RSAKeyMaterial::u() const noexcept
{
    return key_.u;
}

std::unique_ptr<KeyMaterial>
DSAKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new DSAKeyMaterial(*this));
}

void
DSAKeyMaterial::grip_update(rnp::Hash &hash) const
{
    grip_hash_mpi(hash, key_.p, 'p');
    grip_hash_mpi(hash, key_.q, 'q');
    grip_hash_mpi(hash, key_.g, 'g');
    grip_hash_mpi(hash, key_.y, 'y');
}

bool
DSAKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return !key_.validate(ctx.rng, secret_);
}

void
DSAKeyMaterial::clear_secret() noexcept
{
    key_.clear_secret();
    KeyMaterial::clear_secret();
}

bool
DSAKeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    return pkt.get(key_.p) && pkt.get(key_.q) && pkt.get(key_.g) && pkt.get(key_.y);
}

bool
DSAKeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    if (!pkt.get(key_.x)) {
        RNP_LOG("failed to parse dsa secret key data");
        return false;
    }
    secret_ = true;
    return true;
}

void
DSAKeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.p);
    pkt.add(key_.q);
    pkt.add(key_.g);
    pkt.add(key_.y);
}

void
DSAKeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.x);
}

bool
DSAKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    auto &dsa = dynamic_cast<const DSAKeyParams &>(params);
    if (key_.generate(ctx.rng, dsa.bits(), dsa.qbits())) {
        RNP_LOG("failed to generate DSA key");
        return false;
    }
    return finish_generate();
}

rnp_result_t
DSAKeyMaterial::verify(const rnp::SecurityContext &ctx,
                       const SigMaterial &         sig,
                       const rnp::secure_bytes &   hash) const
{
    auto dsa = dynamic_cast<const DSASigMaterial *>(&sig);
    if (!dsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.verify(dsa->sig, hash);
}

rnp_result_t
DSAKeyMaterial::sign(rnp::SecurityContext &   ctx,
                     SigMaterial &            sig,
                     const rnp::secure_bytes &hash) const
{
    auto dsa = dynamic_cast<DSASigMaterial *>(&sig);
    if (!dsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.sign(ctx.rng, dsa->sig, hash);
}

pgp_hash_alg_t
DSAKeyMaterial::adjust_hash(pgp_hash_alg_t hash) const
{
    pgp_hash_alg_t hash_min = dsa::Key::get_min_hash(key_.q.bits());
    if (rnp::Hash::size(hash) < rnp::Hash::size(hash_min)) {
        return hash_min;
    }
    return hash;
}

void
DSAKeyMaterial::set_secret(const mpi &x)
{
    key_.x = x;
    secret_ = true;
}

size_t
DSAKeyMaterial::bits() const noexcept
{
    return 8 * key_.p.size();
}

size_t
DSAKeyMaterial::qbits() const noexcept
{
    return 8 * key_.q.size();
}

const mpi &
DSAKeyMaterial::p() const noexcept
{
    return key_.p;
}

const mpi &
DSAKeyMaterial::q() const noexcept
{
    return key_.q;
}

const mpi &
DSAKeyMaterial::g() const noexcept
{
    return key_.g;
}

const mpi &
DSAKeyMaterial::y() const noexcept
{
    return key_.y;
}

const mpi &
DSAKeyMaterial::x() const noexcept
{
    return key_.x;
}

std::unique_ptr<KeyMaterial>
EGKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new EGKeyMaterial(*this));
}

void
EGKeyMaterial::grip_update(rnp::Hash &hash) const
{
    grip_hash_mpi(hash, key_.p, 'p');
    grip_hash_mpi(hash, key_.g, 'g');
    grip_hash_mpi(hash, key_.y, 'y');
}

bool
EGKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return key_.validate(secret_);
}

void
EGKeyMaterial::clear_secret() noexcept
{
    key_.clear_secret();
    KeyMaterial::clear_secret();
}

bool
EGKeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    return pkt.get(key_.p) && pkt.get(key_.g) && pkt.get(key_.y);
}

bool
EGKeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    if (!pkt.get(key_.x)) {
        RNP_LOG("failed to parse eg secret key data");
        return false;
    }
    secret_ = true;
    return true;
}

void
EGKeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.p);
    pkt.add(key_.g);
    pkt.add(key_.y);
}

void
EGKeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.x);
}

bool
EGKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    /* We do not generate PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN keys */
    if (alg_ != PGP_PKA_ELGAMAL) {
        RNP_LOG("Unsupported algorithm for key generation: %d", alg_);
        return false;
    }
    if (key_.generate(ctx.rng, params.bits())) {
        RNP_LOG("failed to generate ElGamal key");
        return false;
    }
    return finish_generate();
}

rnp_result_t
EGKeyMaterial::encrypt(rnp::SecurityContext &   ctx,
                       EncMaterial &            out,
                       const rnp::secure_bytes &data) const
{
    auto eg = dynamic_cast<EGEncMaterial *>(&out);
    if (!eg) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.encrypt_pkcs1(ctx.rng, eg->enc, data);
}

rnp_result_t
EGKeyMaterial::decrypt(rnp::SecurityContext &ctx,
                       rnp::secure_bytes &   out,
                       const EncMaterial &   in) const
{
    auto eg = dynamic_cast<const EGEncMaterial *>(&in);
    if (!eg) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.decrypt_pkcs1(ctx.rng, out, eg->enc);
}

rnp_result_t
EGKeyMaterial::verify(const rnp::SecurityContext &ctx,
                      const SigMaterial &         sig,
                      const rnp::secure_bytes &   hash) const
{
    RNP_LOG("ElGamal signatures are considered as invalid.");
    return RNP_ERROR_SIGNATURE_INVALID;
}

void
EGKeyMaterial::set_secret(const mpi &x)
{
    key_.x = x;
    secret_ = true;
}

size_t
EGKeyMaterial::bits() const noexcept
{
    return 8 * key_.y.size();
}

const mpi &
EGKeyMaterial::p() const noexcept
{
    return key_.p;
}

const mpi &
EGKeyMaterial::g() const noexcept
{
    return key_.g;
}

const mpi &
EGKeyMaterial::y() const noexcept
{
    return key_.y;
}

const mpi &
EGKeyMaterial::x() const noexcept
{
    return key_.x;
}

void
ECKeyMaterial::grip_update(rnp::Hash &hash) const
{
    grip_hash_ec(hash, key_);
}

void
ECKeyMaterial::clear_secret() noexcept
{
    key_.clear_secret();
    KeyMaterial::clear_secret();
}

rnp_result_t
ECKeyMaterial::check_curve(size_t hash_len) const
{
    auto curve = ec::Curve::get(key_.curve);
    if (!curve) {
        RNP_LOG("Unknown curve");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!curve->supported) {
        RNP_LOG("EC sign: curve %s is not supported.", curve->pgp_name);
        return RNP_ERROR_NOT_SUPPORTED;
    }
    /* "-2" because ECDSA on P-521 must work with SHA-512 digest */
    if (curve->bytes() - 2 > hash_len) {
        RNP_LOG("Message hash too small");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}

bool
ECKeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    if (!pkt.get(key_.curve) || !pkt.get(key_.p)) {
        return false;
    }
    return true;
}

bool
ECKeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    if (!pkt.get(key_.x)) {
        RNP_LOG("failed to parse ecc secret key data");
        return false;
    }
    secret_ = true;
    return true;
}

void
ECKeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.curve);
    pkt.add(key_.p);
}

void
ECKeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.x);
}

bool
ECKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    auto &ecc = dynamic_cast<const ECCKeyParams &>(params);
    if (!ec::Curve::is_supported(ecc.curve())) {
        RNP_LOG("EC generate: curve %d is not supported.", ecc.curve());
        return false;
    }
    if (key_.generate(ctx.rng, alg_, ecc.curve())) {
        RNP_LOG("failed to generate EC key");
        return false;
    }
    key_.curve = ecc.curve();
    return finish_generate();
}

void
ECKeyMaterial::set_secret(const mpi &x)
{
    key_.x = x;
    secret_ = true;
}

size_t
ECKeyMaterial::bits() const noexcept
{
    auto curve_desc = ec::Curve::get(key_.curve);
    return curve_desc ? curve_desc->bitlen : 0;
}

pgp_curve_t
ECKeyMaterial::curve() const noexcept
{
    return key_.curve;
}

const mpi &
ECKeyMaterial::p() const noexcept
{
    return key_.p;
}

const mpi &
ECKeyMaterial::x() const noexcept
{
    return key_.x;
}

bool
ECDSAKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    if (!ec::Curve::is_supported(key_.curve)) {
        /* allow to import key if curve is not supported */
        RNP_LOG("ECDSA validate: curve %d is not supported.", key_.curve);
        return true;
    }
    return !ecdsa::validate_key(ctx.rng, key_, secret_);
}

std::unique_ptr<KeyMaterial>
ECDSAKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new ECDSAKeyMaterial(*this));
}

rnp_result_t
ECDSAKeyMaterial::verify(const rnp::SecurityContext &ctx,
                         const SigMaterial &         sig,
                         const rnp::secure_bytes &   hash) const
{
    if (!ec::Curve::is_supported(key_.curve)) {
        RNP_LOG("Curve %d is not supported.", key_.curve);
        return RNP_ERROR_NOT_SUPPORTED;
    }
    auto ec = dynamic_cast<const ECSigMaterial *>(&sig);
    if (!ec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ecdsa::verify(ec->sig, ec->halg, hash, key_);
}

rnp_result_t
ECDSAKeyMaterial::sign(rnp::SecurityContext &   ctx,
                       SigMaterial &            sig,
                       const rnp::secure_bytes &hash) const
{
    auto ret = check_curve(hash.size());
    if (ret) {
        return ret;
    }
    auto ec = dynamic_cast<ECSigMaterial *>(&sig);
    if (!ec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ecdsa::sign(ctx.rng, ec->sig, ec->halg, hash, key_);
}

pgp_hash_alg_t
ECDSAKeyMaterial::adjust_hash(pgp_hash_alg_t hash) const
{
    pgp_hash_alg_t hash_min = ecdsa::get_min_hash(key_.curve);
    if (rnp::Hash::size(hash) < rnp::Hash::size(hash_min)) {
        return hash_min;
    }
    return hash;
}

bool
ECDHKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    if (!ec::Curve::is_supported(key_.curve)) {
        /* allow to import key if curve is not supported */
        RNP_LOG("ECDH validate: curve %d is not supported.", key_.curve);
        return true;
    }
    return !ecdh::validate_key(ctx.rng, key_, secret_);
}

std::unique_ptr<KeyMaterial>
ECDHKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new ECDHKeyMaterial(*this));
}

bool
ECDHKeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    if (!ECKeyMaterial::parse(pkt)) {
        return false;
    }
    /* Additional ECDH fields */
    /* Read KDF parameters. At the moment should be 0x03 0x01 halg ealg */
    uint8_t len = 0, halg = 0, walg = 0;
    if (!pkt.get(len) || (len != 3)) {
        return false;
    }
    if (!pkt.get(len) || (len != 1)) {
        return false;
    }
    if (!pkt.get(halg) || !pkt.get(walg)) {
        return false;
    }
    key_.kdf_hash_alg = (pgp_hash_alg_t) halg;
    key_.key_wrap_alg = (pgp_symm_alg_t) walg;
    return true;
}

void
ECDHKeyMaterial::write(pgp_packet_body_t &pkt) const
{
    ECKeyMaterial::write(pkt);
    pkt.add_byte(3);
    pkt.add_byte(1);
    pkt.add_byte(key_.kdf_hash_alg);
    pkt.add_byte(key_.key_wrap_alg);
}

bool
ECDHKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    auto &ecc = dynamic_cast<const ECCKeyParams &>(params);
    if (!ecdh::set_params(key_, ecc.curve())) {
        RNP_LOG("Unsupported curve [ID=%d]", ecc.curve());
        return false;
    }
    /* Special case for x25519*/
    if (ecc.curve() == PGP_CURVE_25519) {
        if (key_.generate_x25519(ctx.rng)) {
            RNP_LOG("failed to generate x25519 key");
            return false;
        }
        key_.curve = ecc.curve();
        return finish_generate();
    }
    /* Fallback to default EC generation for other cases */
    return ECKeyMaterial::generate(ctx, params);
}

rnp_result_t
ECDHKeyMaterial::encrypt(rnp::SecurityContext &   ctx,
                         EncMaterial &            out,
                         const rnp::secure_bytes &data) const
{
    if (!ec::Curve::is_supported(key_.curve)) {
        RNP_LOG("ECDH encrypt: curve %d is not supported.", key_.curve);
        return RNP_ERROR_NOT_SUPPORTED;
    }
    auto ecdh = dynamic_cast<ECDHEncMaterial *>(&out);
    if (!ecdh) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ecdh::encrypt_pkcs5(ctx.rng, ecdh->enc, data, key_);
}

rnp_result_t
ECDHKeyMaterial::decrypt(rnp::SecurityContext &ctx,
                         rnp::secure_bytes &   out,
                         const EncMaterial &   in) const
{
    if (!ec::Curve::is_supported(key_.curve)) {
        RNP_LOG("ECDH decrypt: curve %d is not supported.", key_.curve);
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if ((key_.curve == PGP_CURVE_25519) && !x25519_bits_tweaked()) {
        RNP_LOG("Warning: bits of 25519 secret key are not tweaked.");
    }
    auto ecdh = dynamic_cast<const ECDHEncMaterial *>(&in);
    if (!ecdh) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ecdh::decrypt_pkcs5(out, ecdh->enc, key_);
}

pgp_hash_alg_t
ECDHKeyMaterial::kdf_hash_alg() const noexcept
{
    return key_.kdf_hash_alg;
}

pgp_symm_alg_t
ECDHKeyMaterial::key_wrap_alg() const noexcept
{
    return key_.key_wrap_alg;
}

bool
ECDHKeyMaterial::x25519_bits_tweaked() const noexcept
{
    return (key_.curve == PGP_CURVE_25519) && ::x25519_bits_tweaked(key_);
}

bool
ECDHKeyMaterial::x25519_tweak_bits() noexcept
{
    return (key_.curve == PGP_CURVE_25519) && ::x25519_tweak_bits(key_);
}

bool
EDDSAKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return !eddsa::validate_key(ctx.rng, key_, secret_);
}

std::unique_ptr<KeyMaterial>
EDDSAKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new EDDSAKeyMaterial(*this));
}

bool
EDDSAKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    if (eddsa::generate(ctx.rng, key_)) {
        RNP_LOG("failed to generate EDDSA key");
        return false;
    }
    return finish_generate();
}

rnp_result_t
EDDSAKeyMaterial::verify(const rnp::SecurityContext &ctx,
                         const SigMaterial &         sig,
                         const rnp::secure_bytes &   hash) const
{
    auto ec = dynamic_cast<const ECSigMaterial *>(&sig);
    if (!ec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return eddsa::verify(ec->sig, hash, key_);
}

rnp_result_t
EDDSAKeyMaterial::sign(rnp::SecurityContext &   ctx,
                       SigMaterial &            sig,
                       const rnp::secure_bytes &hash) const
{
    auto ec = dynamic_cast<ECSigMaterial *>(&sig);
    if (!ec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return eddsa::sign(ctx.rng, ec->sig, hash, key_);
}

bool
SM2KeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
#if defined(ENABLE_SM2)
    return !sm2::validate_key(ctx.rng, key_, secret_);
#else
    RNP_LOG("SM2 key validation is not available.");
    return false;
#endif
}

std::unique_ptr<KeyMaterial>
SM2KeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new SM2KeyMaterial(*this));
}

rnp_result_t
SM2KeyMaterial::encrypt(rnp::SecurityContext &   ctx,
                        EncMaterial &            out,
                        const rnp::secure_bytes &data) const
{
#if defined(ENABLE_SM2)
    auto sm2 = dynamic_cast<SM2EncMaterial *>(&out);
    if (!sm2) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return sm2::encrypt(ctx.rng, sm2->enc, data, PGP_HASH_SM3, key_);
#else
    RNP_LOG("sm2_encrypt is not available");
    return RNP_ERROR_NOT_IMPLEMENTED;
#endif
}

rnp_result_t
SM2KeyMaterial::decrypt(rnp::SecurityContext &ctx,
                        rnp::secure_bytes &   out,
                        const EncMaterial &   in) const
{
#if defined(ENABLE_SM2)
    auto sm2 = dynamic_cast<const SM2EncMaterial *>(&in);
    if (!sm2) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return sm2::decrypt(out, sm2->enc, key_);
#else
    RNP_LOG("SM2 decryption is not available.");
    return RNP_ERROR_NOT_IMPLEMENTED;
#endif
}

rnp_result_t
SM2KeyMaterial::verify(const rnp::SecurityContext &ctx,
                       const SigMaterial &         sig,
                       const rnp::secure_bytes &   hash) const
{
#if defined(ENABLE_SM2)
    auto ec = dynamic_cast<const ECSigMaterial *>(&sig);
    if (!ec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return sm2::verify(ec->sig, ec->halg, hash, key_);
#else
    RNP_LOG("SM2 verification is not available.");
    return RNP_ERROR_NOT_IMPLEMENTED;
#endif
}

rnp_result_t
SM2KeyMaterial::sign(rnp::SecurityContext &   ctx,
                     SigMaterial &            sig,
                     const rnp::secure_bytes &hash) const
{
#if defined(ENABLE_SM2)
    auto ret = check_curve(hash.size());
    if (ret) {
        return ret;
    }
    auto ec = dynamic_cast<ECSigMaterial *>(&sig);
    if (!ec) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return sm2::sign(ctx.rng, ec->sig, ec->halg, hash, key_);
#else
    RNP_LOG("SM2 signing is not available.");
    return RNP_ERROR_NOT_IMPLEMENTED;
#endif
}

void
SM2KeyMaterial::compute_za(rnp::Hash &hash) const
{
#if defined(ENABLE_SM2)
    auto res = sm2::compute_za(key_, hash);
    if (res) {
        RNP_LOG("failed to compute SM2 ZA field");
        throw rnp::rnp_exception(res);
    }
#else
    RNP_LOG("SM2 ZA computation not available");
    throw rnp::rnp_exception(RNP_ERROR_NOT_IMPLEMENTED);
#endif
}

#if defined(ENABLE_CRYPTO_REFRESH)
std::unique_ptr<KeyMaterial>
Ed25519KeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new Ed25519KeyMaterial(*this));
}

void
Ed25519KeyMaterial::grip_update(rnp::Hash &hash) const
{
    // TODO: if GnuPG would ever support v6, check whether this works correctly.
    hash.add(pub());
}

bool
Ed25519KeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return !ed25519_validate_key_native(&ctx.rng, &key_, secret_);
}

void
Ed25519KeyMaterial::clear_secret() noexcept
{
    key_.clear_secret();
    KeyMaterial::clear_secret();
}

bool
Ed25519KeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    auto                 ec_desc = ec::Curve::get(PGP_CURVE_ED25519);
    std::vector<uint8_t> buf(ec_desc->bytes());
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse Ed25519 public key data");
        return false;
    }
    key_.pub = buf;
    return true;
}

bool
Ed25519KeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    auto                 ec_desc = ec::Curve::get(PGP_CURVE_ED25519);
    std::vector<uint8_t> buf(ec_desc->bytes());
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse Ed25519 secret key data");
        return false;
    }
    key_.priv = buf;
    secret_ = true;
    return true;
}

void
Ed25519KeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.pub);
}

void
Ed25519KeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.priv);
}

bool
Ed25519KeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    if (generate_ed25519_native(&ctx.rng, key_.priv, key_.pub)) {
        RNP_LOG("failed to generate ED25519 key");
        return false;
    }
    return finish_generate();
}

rnp_result_t
Ed25519KeyMaterial::verify(const rnp::SecurityContext &ctx,
                           const SigMaterial &         sig,
                           const rnp::secure_bytes &   hash) const
{
    auto ed25519 = dynamic_cast<const Ed25519SigMaterial *>(&sig);
    if (!ed25519) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ed25519_verify_native(ed25519->sig.sig, key_.pub, hash.data(), hash.size());
}

rnp_result_t
Ed25519KeyMaterial::sign(rnp::SecurityContext &   ctx,
                         SigMaterial &            sig,
                         const rnp::secure_bytes &hash) const
{
    auto ed25519 = dynamic_cast<Ed25519SigMaterial *>(&sig);
    if (!ed25519) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ed25519_sign_native(
      &ctx.rng, ed25519->sig.sig, key_.priv, hash.data(), hash.size());
}

size_t
Ed25519KeyMaterial::bits() const noexcept
{
    return 255;
}

pgp_curve_t
Ed25519KeyMaterial::curve() const noexcept
{
    return PGP_CURVE_ED25519;
}

const std::vector<uint8_t> &
Ed25519KeyMaterial::pub() const noexcept
{
    return key_.pub;
}

const std::vector<uint8_t> &
Ed25519KeyMaterial::priv() const noexcept
{
    return key_.priv;
}

std::unique_ptr<KeyMaterial>
X25519KeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new X25519KeyMaterial(*this));
}

void
X25519KeyMaterial::grip_update(rnp::Hash &hash) const
{
    // TODO: if GnuPG would ever support v6, check whether this works correctly.
    hash.add(pub());
}

bool
X25519KeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return !x25519_validate_key_native(&ctx.rng, &key_, secret_);
}

void
X25519KeyMaterial::clear_secret() noexcept
{
    key_.clear_secret();
    KeyMaterial::clear_secret();
}

bool
X25519KeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    auto                 ec_desc = ec::Curve::get(PGP_CURVE_25519);
    std::vector<uint8_t> buf(ec_desc->bytes());
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse X25519 public key data");
        return false;
    }
    key_.pub = buf;
    return true;
}

bool
X25519KeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    auto                 ec_desc = ec::Curve::get(PGP_CURVE_25519);
    std::vector<uint8_t> buf(ec_desc->bytes());
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse X25519 secret key data");
        return false;
    }
    key_.priv = buf;
    secret_ = true;
    return true;
}

void
X25519KeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.pub);
}

void
X25519KeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.priv);
}

bool
X25519KeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    if (generate_x25519_native(&ctx.rng, key_.priv, key_.pub)) {
        RNP_LOG("failed to generate X25519 key");
        return false;
    }
    return finish_generate();
}

rnp_result_t
X25519KeyMaterial::encrypt(rnp::SecurityContext &   ctx,
                           EncMaterial &            out,
                           const rnp::secure_bytes &data) const
{
    auto x25519 = dynamic_cast<X25519EncMaterial *>(&out);
    if (!x25519) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return x25519_native_encrypt(&ctx.rng, key_.pub, data.data(), data.size(), &x25519->enc);
}

rnp_result_t
X25519KeyMaterial::decrypt(rnp::SecurityContext &ctx,
                           rnp::secure_bytes &   out,
                           const EncMaterial &   in) const
{
    auto x25519 = dynamic_cast<const X25519EncMaterial *>(&in);
    if (!x25519) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    out.resize(PGP_MPINT_SIZE);
    size_t out_size = out.size();
    auto   ret = x25519_native_decrypt(&ctx.rng, key_, &x25519->enc, out.data(), &out_size);
    if (!ret) {
        out.resize(out_size);
    }
    return ret;
}

size_t
X25519KeyMaterial::bits() const noexcept
{
    return 255;
}

pgp_curve_t
X25519KeyMaterial::curve() const noexcept
{
    return PGP_CURVE_25519;
}

const std::vector<uint8_t> &
X25519KeyMaterial::pub() const noexcept
{
    return key_.pub;
}

const std::vector<uint8_t> &
X25519KeyMaterial::priv() const noexcept
{
    return key_.priv;
}
#endif

#if defined(ENABLE_PQC)
std::unique_ptr<KeyMaterial>
MlkemEcdhKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new MlkemEcdhKeyMaterial(*this));
}

void
MlkemEcdhKeyMaterial::grip_update(rnp::Hash &hash) const
{
    hash.add(pub().get_encoded());
}

bool
MlkemEcdhKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return !kyber_ecdh_validate_key(&ctx.rng, &key_, secret_);
}

void
MlkemEcdhKeyMaterial::clear_secret() noexcept
{
    key_.priv.secure_clear();
    KeyMaterial::clear_secret();
}

bool
MlkemEcdhKeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    std::vector<uint8_t> buf(pgp_kyber_ecdh_composite_public_key_t::encoded_size(alg()));
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse mlkem-ecdh public key data");
        return false;
    }
    key_.pub = pgp_kyber_ecdh_composite_public_key_t(buf, alg());
    return true;
}

bool
MlkemEcdhKeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    std::vector<uint8_t> buf(pgp_kyber_ecdh_composite_private_key_t::encoded_size(alg()));
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse mkem-ecdh secret key data");
        return false;
    }
    key_.priv = pgp_kyber_ecdh_composite_private_key_t(buf.data(), buf.size(), alg());
    secret_ = true;
    return true;
}

void
MlkemEcdhKeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.pub.get_encoded());
}

void
MlkemEcdhKeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.priv.get_encoded());
}

bool
MlkemEcdhKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    if (pgp_kyber_ecdh_composite_key_t::gen_keypair(&ctx.rng, &key_, alg_)) {
        RNP_LOG("failed to generate MLKEM-ECDH-composite key for PK alg %d", alg_);
        return false;
    }
    return finish_generate();
}

rnp_result_t
MlkemEcdhKeyMaterial::encrypt(rnp::SecurityContext &   ctx,
                              EncMaterial &            out,
                              const rnp::secure_bytes &data) const
{
    auto mlkem = dynamic_cast<MlkemEcdhEncMaterial *>(&out);
    if (!mlkem) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.pub.encrypt(&ctx.rng, &mlkem->enc, data.data(), data.size());
}

rnp_result_t
MlkemEcdhKeyMaterial::decrypt(rnp::SecurityContext &ctx,
                              rnp::secure_bytes &   out,
                              const EncMaterial &   in) const
{
    auto mlkem = dynamic_cast<const MlkemEcdhEncMaterial *>(&in);
    if (!mlkem) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    out.resize(PGP_MPINT_SIZE);
    size_t out_size = out.size();
    auto   ret = key_.priv.decrypt(&ctx.rng, out.data(), &out_size, &mlkem->enc);
    if (!ret) {
        out.resize(out_size);
    }
    return ret;
}

size_t
MlkemEcdhKeyMaterial::bits() const noexcept
{
    return 8 * pub().get_encoded().size(); /* public key length */
}

const pgp_kyber_ecdh_composite_public_key_t &
MlkemEcdhKeyMaterial::pub() const noexcept
{
    return key_.pub;
}

const pgp_kyber_ecdh_composite_private_key_t &
MlkemEcdhKeyMaterial::priv() const noexcept
{
    return key_.priv;
}

std::unique_ptr<KeyMaterial>
DilithiumEccKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new DilithiumEccKeyMaterial(*this));
}

void
DilithiumEccKeyMaterial::grip_update(rnp::Hash &hash) const
{
    hash.add(pub().get_encoded());
}

bool
DilithiumEccKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return !dilithium_exdsa_validate_key(&ctx.rng, &key_, secret_);
}

void
DilithiumEccKeyMaterial::clear_secret() noexcept
{
    key_.priv.secure_clear();
    KeyMaterial::clear_secret();
}

bool
DilithiumEccKeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    std::vector<uint8_t> buf(pgp_dilithium_exdsa_composite_public_key_t::encoded_size(alg()));
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse mldsa-ecdsa/eddsa public key data");
        return false;
    }
    key_.pub = pgp_dilithium_exdsa_composite_public_key_t(buf, alg());
    return true;
}

bool
DilithiumEccKeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    std::vector<uint8_t> buf(pgp_dilithium_exdsa_composite_private_key_t::encoded_size(alg()));
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse mldsa-ecdsa/eddsa secret key data");
        return false;
    }
    key_.priv = pgp_dilithium_exdsa_composite_private_key_t(buf.data(), buf.size(), alg());
    secret_ = true;
    return true;
}

void
DilithiumEccKeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.pub.get_encoded());
}

void
DilithiumEccKeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add(key_.priv.get_encoded());
}

bool
DilithiumEccKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    if (pgp_dilithium_exdsa_composite_key_t::gen_keypair(&ctx.rng, &key_, alg_)) {
        RNP_LOG("failed to generate mldsa-ecdsa/eddsa-composite key for PK alg %d", alg_);
        return false;
    }
    return finish_generate();
}

rnp_result_t
DilithiumEccKeyMaterial::verify(const rnp::SecurityContext &ctx,
                                const SigMaterial &         sig,
                                const rnp::secure_bytes &   hash) const
{
    auto dilithium = dynamic_cast<const DilithiumSigMaterial *>(&sig);
    if (!dilithium) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.pub.verify(&dilithium->sig, dilithium->halg, hash.data(), hash.size());
}

rnp_result_t
DilithiumEccKeyMaterial::sign(rnp::SecurityContext &   ctx,
                              SigMaterial &            sig,
                              const rnp::secure_bytes &hash) const
{
    auto dilithium = dynamic_cast<DilithiumSigMaterial *>(&sig);
    if (!dilithium) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.priv.sign(
      &ctx.rng, &dilithium->sig, dilithium->halg, hash.data(), hash.size());
}

pgp_hash_alg_t
DilithiumEccKeyMaterial::adjust_hash(pgp_hash_alg_t hash) const
{
    return dilithium_default_hash_alg();
}

size_t
DilithiumEccKeyMaterial::bits() const noexcept
{
    return 8 * pub().get_encoded().size(); /* public key length*/
}

const pgp_dilithium_exdsa_composite_public_key_t &
DilithiumEccKeyMaterial::pub() const noexcept
{
    return key_.pub;
}

const pgp_dilithium_exdsa_composite_private_key_t &
DilithiumEccKeyMaterial::priv() const noexcept
{
    return key_.priv;
}

std::unique_ptr<KeyMaterial>
SlhdsaKeyMaterial::clone()
{
    return std::unique_ptr<KeyMaterial>(new SlhdsaKeyMaterial(*this));
}

void
SlhdsaKeyMaterial::grip_update(rnp::Hash &hash) const
{
    hash.add(pub().get_encoded());
}

bool
SlhdsaKeyMaterial::validate_material(rnp::SecurityContext &ctx, bool reset)
{
    return !sphincsplus_validate_key(&ctx.rng, &key_, secret_);
}

void
SlhdsaKeyMaterial::clear_secret() noexcept
{
    key_.priv.secure_clear();
    KeyMaterial::clear_secret();
}

bool
SlhdsaKeyMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    secret_ = false;
    uint8_t bt = 0;
    if (!pkt.get(bt)) {
        RNP_LOG("failed to parse SLH-DSA public key data");
        return false;
    }
    sphincsplus_parameter_t param = (sphincsplus_parameter_t) bt;
    auto                    size = sphincsplus_pubkey_size(param);
    if (!size) {
        RNP_LOG("invalid SLH-DSA param");
        return false;
    }
    std::vector<uint8_t> buf(size);
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse SLH-DSA public key data");
        return false;
    }
    key_.pub = pgp_sphincsplus_public_key_t(buf, param, alg());
    return true;
}

bool
SlhdsaKeyMaterial::parse_secret(pgp_packet_body_t &pkt) noexcept
{
    uint8_t bt = 0;
    if (!pkt.get(bt)) {
        RNP_LOG("failed to parse SLH-DSA secret key data");
        return false;
    }
    sphincsplus_parameter_t param = (sphincsplus_parameter_t) bt;
    std::vector<uint8_t>    buf(sphincsplus_privkey_size(param));
    if (!pkt.get(buf.data(), buf.size())) {
        RNP_LOG("failed to parse SLH-DSA secret key data");
        return false;
    }
    key_.priv = pgp_sphincsplus_private_key_t(buf, param, alg());
    secret_ = true;
    return true;
}

void
SlhdsaKeyMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add_byte((uint8_t) key_.pub.param());
    pkt.add(key_.pub.get_encoded());
}

void
SlhdsaKeyMaterial::write_secret(pgp_packet_body_t &pkt) const
{
    pkt.add_byte((uint8_t) key_.priv.param());
    pkt.add(key_.priv.get_encoded());
}

bool
SlhdsaKeyMaterial::generate(rnp::SecurityContext &ctx, const KeyParams &params)
{
    auto &slhdsa = dynamic_cast<const SlhdsaKeyParams &>(params);
    if (pgp_sphincsplus_generate(&ctx.rng, &key_, slhdsa.param(), alg_)) {
        RNP_LOG("failed to generate SLH-DSA key for PK alg %d", alg_);
        return false;
    }
    return finish_generate();
}

rnp_result_t
SlhdsaKeyMaterial::verify(const rnp::SecurityContext &ctx,
                          const SigMaterial &         sig,
                          const rnp::secure_bytes &   hash) const
{
    auto slhdsa = dynamic_cast<const SlhdsaSigMaterial *>(&sig);
    if (!slhdsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.pub.verify(&slhdsa->sig, hash.data(), hash.size());
}

rnp_result_t
SlhdsaKeyMaterial::sign(rnp::SecurityContext &   ctx,
                        SigMaterial &            sig,
                        const rnp::secure_bytes &hash) const
{
    auto slhdsa = dynamic_cast<SlhdsaSigMaterial *>(&sig);
    if (!slhdsa) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return key_.priv.sign(&ctx.rng, &slhdsa->sig, hash.data(), hash.size());
}

pgp_hash_alg_t
SlhdsaKeyMaterial::adjust_hash(pgp_hash_alg_t hash) const
{
    return sphincsplus_default_hash_alg(alg_, key_.pub.param());
}

bool
SlhdsaKeyMaterial::sig_hash_allowed(pgp_hash_alg_t hash) const
{
    return key_.pub.validate_signature_hash_requirements(hash);
}

size_t
SlhdsaKeyMaterial::bits() const noexcept
{
    return 8 * pub().get_encoded().size(); /* public key length */
}

const pgp_sphincsplus_public_key_t &
SlhdsaKeyMaterial::pub() const noexcept
{
    return key_.pub;
}

const pgp_sphincsplus_private_key_t &
SlhdsaKeyMaterial::priv() const noexcept
{
    return key_.priv;
}
#endif

} // namespace pgp
