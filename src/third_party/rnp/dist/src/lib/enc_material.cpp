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

#include "enc_material.hpp"
#include "librepgp/stream-packet.h"
#include "logging.h"

namespace pgp {

std::unique_ptr<EncMaterial>
EncMaterial::create(pgp_pubkey_alg_t alg)
{
    switch (alg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_ENCRYPT_ONLY:
        return std::unique_ptr<EncMaterial>(new RSAEncMaterial());
    case PGP_PKA_ELGAMAL:
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN:
        return std::unique_ptr<EncMaterial>(new EGEncMaterial());
    case PGP_PKA_SM2:
        return std::unique_ptr<EncMaterial>(new SM2EncMaterial());
    case PGP_PKA_ECDH:
        return std::unique_ptr<EncMaterial>(new ECDHEncMaterial());
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_X25519:
        return std::unique_ptr<EncMaterial>(new X25519EncMaterial());
#endif
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
        return std::unique_ptr<EncMaterial>(new MlkemEcdhEncMaterial(alg));
#endif
    default:
        RNP_LOG("unknown pk alg %d", alg);
        return nullptr;
    }
}

bool
RSAEncMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    return pkt.get(enc.m);
}

void
RSAEncMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(enc.m);
}

bool
EGEncMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    return pkt.get(enc.g) && pkt.get(enc.m);
}

void
EGEncMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(enc.g);
    pkt.add(enc.m);
}

bool
SM2EncMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    return pkt.get(enc.m);
}

void
SM2EncMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(enc.m);
}

bool
ECDHEncMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    uint8_t sz = 0;
    /* ECDH ephemeral point and m size */
    if (!pkt.get(enc.p) || !pkt.get(sz)) {
        return false;
    }
    /* ECDH m */
    if (sz > ECDH_WRAPPED_KEY_SIZE) {
        RNP_LOG("wrong ecdh m len");
        return false;
    }
    enc.m.resize(sz);
    if (!pkt.get(enc.m.data(), sz)) {
        return false;
    }
    return true;
}

void
ECDHEncMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(enc.p);
    pkt.add_byte((uint8_t) enc.m.size());
    pkt.add(enc.m);
}

#if defined(ENABLE_CRYPTO_REFRESH)
bool
X25519EncMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    enc.eph_key.resize(32);
    if (!pkt.get(enc.eph_key.data(), enc.eph_key.size())) {
        RNP_LOG("failed to parse X25519 PKESK (eph. pubkey)");
        return false;
    }
    uint8_t sess_len = 0;
    if (!pkt.get(sess_len) || !sess_len) {
        RNP_LOG("failed to parse X25519 PKESK (enc sesskey length)");
        return false;
    }
    /* get plaintext salg if PKESKv3 */
    if (version == PGP_PKSK_V3) {
        uint8_t bt = 0;
        if (!pkt.get(bt)) {
            RNP_LOG("failed to get salg");
            return RNP_ERROR_BAD_FORMAT;
        }
        sess_len--;
        salg = (pgp_symm_alg_t) bt;
    }
    enc.enc_sess_key.resize(sess_len);
    if (!pkt.get(enc.enc_sess_key.data(), sess_len)) {
        RNP_LOG("failed to parse X25519 PKESK (enc sesskey)");
        return false;
    }
    return true;
}

void
X25519EncMaterial::write(pgp_packet_body_t &pkt) const
{
    uint8_t inc = ((version == PGP_PKSK_V3) ? 1 : 0);
    pkt.add(enc.eph_key);
    pkt.add_byte(static_cast<uint8_t>(enc.enc_sess_key.size() + inc));
    if (version == PGP_PKSK_V3) {
        pkt.add_byte(salg); /* added as plaintext */
    }
    pkt.add(enc.enc_sess_key);
}
#endif

#if defined(ENABLE_PQC)
bool
MlkemEcdhEncMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    /* Calculate ciphertext size */
    auto csize =
      kyber_ciphertext_size(pgp_kyber_ecdh_composite_key_t::pk_alg_to_kyber_id(alg_)) +
      pgp_kyber_ecdh_composite_key_t::ecdh_curve_ephemeral_size(
        pgp_kyber_ecdh_composite_key_t::pk_alg_to_curve_id(alg_));
    enc.composite_ciphertext.resize(csize);
    /* Read composite ciphertext */
    if (!pkt.get(enc.composite_ciphertext.data(), enc.composite_ciphertext.size())) {
        RNP_LOG("failed to get kyber-ecdh ciphertext");
        return false;
    }
    uint8_t wrapped_key_len = 0;
    if (!pkt.get(wrapped_key_len) || !wrapped_key_len) {
        RNP_LOG("failed to get kyber-ecdh wrapped session key length");
        return false;
    }
    /* get plaintext salg if PKESKv3 */
    if (version == PGP_PKSK_V3) {
        uint8_t balg = 0;
        if (!pkt.get(balg)) {
            RNP_LOG("failed to get salg");
            return RNP_ERROR_BAD_FORMAT;
        }
        salg = (pgp_symm_alg_t) balg;
        wrapped_key_len--;
    }
    enc.wrapped_sesskey.resize(wrapped_key_len);
    if (!pkt.get(enc.wrapped_sesskey.data(), enc.wrapped_sesskey.size())) {
        RNP_LOG("failed to get kyber-ecdh session key");
        return false;
    }
    return true;
}

void
MlkemEcdhEncMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(enc.composite_ciphertext);
    uint8_t inc = ((version == PGP_PKSK_V3) ? 1 : 0);
    pkt.add_byte(static_cast<uint8_t>(enc.wrapped_sesskey.size()) + inc);
    if (version == PGP_PKSK_V3) {
        pkt.add_byte(salg); /* added as plaintext */
    }
    pkt.add(enc.wrapped_sesskey);
}
#endif

} // namespace pgp
