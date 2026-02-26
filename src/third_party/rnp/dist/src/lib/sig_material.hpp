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

#ifndef RNP_SIG_MATERIAL_HPP_
#define RNP_SIG_MATERIAL_HPP_

#include "types.h"
#include "defaults.h"

typedef struct pgp_packet_body_t pgp_packet_body_t;

namespace pgp {

class SigMaterial {
  public:
    pgp_hash_alg_t halg;
    SigMaterial(pgp_hash_alg_t ahalg) : halg(ahalg){};
    virtual ~SigMaterial(){};

    virtual bool parse(pgp_packet_body_t &pkt) noexcept = 0;
    virtual void write(pgp_packet_body_t &pkt) const = 0;

    static std::unique_ptr<SigMaterial> create(pgp_pubkey_alg_t alg, pgp_hash_alg_t halg);
};

class RSASigMaterial : public SigMaterial {
  public:
    rsa::Signature sig;
    RSASigMaterial(pgp_hash_alg_t ahalg) : SigMaterial(ahalg){};

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

class DSASigMaterial : public SigMaterial {
  public:
    dsa::Signature sig;
    DSASigMaterial(pgp_hash_alg_t ahalg) : SigMaterial(ahalg){};

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

class EGSigMaterial : public SigMaterial {
  public:
    eg::Signature sig;
    EGSigMaterial(pgp_hash_alg_t ahalg) : SigMaterial(ahalg){};

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

class ECSigMaterial : public SigMaterial {
  public:
    ec::Signature sig;
    ECSigMaterial(pgp_hash_alg_t ahalg) : SigMaterial(ahalg){};

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

#if defined(ENABLE_CRYPTO_REFRESH)
class Ed25519SigMaterial : public SigMaterial {
  public:
    pgp_ed25519_signature_t sig;
    Ed25519SigMaterial(pgp_hash_alg_t ahalg) : SigMaterial(ahalg){};

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};
#endif

#if defined(ENABLE_PQC)
class DilithiumSigMaterial : public SigMaterial {
  public:
    pgp_pubkey_alg_t                palg;
    pgp_dilithium_exdsa_signature_t sig;

    DilithiumSigMaterial(pgp_pubkey_alg_t apalg, pgp_hash_alg_t ahalg)
        : SigMaterial(ahalg), palg(apalg){};

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

class SlhdsaSigMaterial : public SigMaterial {
  public:
    pgp_sphincsplus_signature_t sig;
    SlhdsaSigMaterial(pgp_hash_alg_t ahalg) : SigMaterial(ahalg){};

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};
#endif
} // namespace pgp

#endif // RNP_SIG_MATERIAL_HPP_
