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

#include "sig_material.hpp"
#include "librepgp/stream-packet.h"
#include "logging.h"

namespace pgp {

std::unique_ptr<SigMaterial>
SigMaterial::create(pgp_pubkey_alg_t palg, pgp_hash_alg_t halg)
{
    switch (palg) {
    case PGP_PKA_RSA:
    case PGP_PKA_RSA_SIGN_ONLY:
        return std::unique_ptr<SigMaterial>(new RSASigMaterial(halg));
    case PGP_PKA_DSA:
        return std::unique_ptr<SigMaterial>(new DSASigMaterial(halg));
    case PGP_PKA_EDDSA:
    case PGP_PKA_ECDSA:
    case PGP_PKA_SM2:
    case PGP_PKA_ECDH:
        return std::unique_ptr<SigMaterial>(new ECSigMaterial(halg));
    case PGP_PKA_ELGAMAL: /* we support reading it but will not validate */
    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN:
        return std::unique_ptr<SigMaterial>(new EGSigMaterial(halg));
#if defined(ENABLE_CRYPTO_REFRESH)
    case PGP_PKA_ED25519: {
        return std::unique_ptr<SigMaterial>(new Ed25519SigMaterial(halg));
    }
#endif
#if defined(ENABLE_PQC)
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
        return std::unique_ptr<SigMaterial>(new DilithiumSigMaterial(palg, halg));
    case PGP_PKA_SPHINCSPLUS_SHA2:
        FALLTHROUGH_STATEMENT;
    case PGP_PKA_SPHINCSPLUS_SHAKE:
        return std::unique_ptr<SigMaterial>(new SlhdsaSigMaterial(halg));
#endif
    default:
        RNP_LOG("Unknown pk algorithm : %d", (int) palg);
        return nullptr;
    }
}

bool
RSASigMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    return pkt.get(sig.s);
}

void
RSASigMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(sig.s);
}

bool
DSASigMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    return pkt.get(sig.r) && pkt.get(sig.s);
}

void
DSASigMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(sig.r);
    pkt.add(sig.s);
}

bool
EGSigMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    return pkt.get(sig.r) && pkt.get(sig.s);
}

void
EGSigMaterial::write(pgp_packet_body_t &pkt) const
{
    /* we support writing it but will not generate */
    pkt.add(sig.r);
    pkt.add(sig.s);
}

bool
ECSigMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    return pkt.get(sig.r) && pkt.get(sig.s);
}

void
ECSigMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(sig.r);
    pkt.add(sig.s);
}

#if defined(ENABLE_CRYPTO_REFRESH)
bool
Ed25519SigMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    auto ec_desc = pgp::ec::Curve::get(PGP_CURVE_25519);
    if (!pkt.get(sig.sig, 2 * ec_desc->bytes())) {
        RNP_LOG("failed to parse ED25519 signature data");
        return false;
    }
    return true;
}

void
Ed25519SigMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(sig.sig);
}
#endif

#if defined(ENABLE_PQC)
bool
DilithiumSigMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    if (!pkt.get(sig.sig, pgp_dilithium_exdsa_signature_t::composite_signature_size(palg))) {
        RNP_LOG("failed to get mldsa-ecdsa/eddsa signature");
        return false;
    }
    return true;
}

void
DilithiumSigMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add(sig.sig);
}

bool
SlhdsaSigMaterial::parse(pgp_packet_body_t &pkt) noexcept
{
    uint8_t param = 0;
    if (!pkt.get(param)) {
        RNP_LOG("failed to parse SLH-DSA signature data");
        return false;
    }
    auto sig_size = sphincsplus_signature_size((sphincsplus_parameter_t) param);
    if (!sig_size) {
        RNP_LOG("invalid SLH-DSA param value");
        return false;
    }
    sig.param = (sphincsplus_parameter_t) param;
    if (!pkt.get(sig.sig, sig_size)) {
        RNP_LOG("failed to parse SLH-DSA signature data");
        return false;
    }
    return true;
}

void
SlhdsaSigMaterial::write(pgp_packet_body_t &pkt) const
{
    pkt.add_byte((uint8_t) sig.param);
    pkt.add(sig.sig);
}
#endif

} // namespace pgp
