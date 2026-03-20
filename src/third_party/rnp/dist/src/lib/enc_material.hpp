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

#ifndef RNP_ENC_MATERIAL_HPP_
#define RNP_ENC_MATERIAL_HPP_

#include "types.h"
#include "defaults.h"

typedef struct pgp_packet_body_t pgp_packet_body_t;

namespace pgp {

class EncMaterial {
  public:
#if defined(ENABLE_CRYPTO_REFRESH)
    pgp_pkesk_version_t version = PGP_PKSK_V3;
    pgp_symm_alg_t      salg = PGP_SA_UNKNOWN;
#endif
    virtual ~EncMaterial(){};

    virtual bool parse(pgp_packet_body_t &pkt) noexcept = 0;
    virtual void write(pgp_packet_body_t &pkt) const = 0;

    static std::unique_ptr<EncMaterial> create(pgp_pubkey_alg_t alg);
};

class RSAEncMaterial : public EncMaterial {
  public:
    rsa::Encrypted enc;

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

class EGEncMaterial : public EncMaterial {
  public:
    eg::Encrypted enc;

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

class SM2EncMaterial : public EncMaterial {
  public:
    sm2::Encrypted enc;

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

class ECDHEncMaterial : public EncMaterial {
  public:
    ecdh::Encrypted enc;

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};

#if defined(ENABLE_CRYPTO_REFRESH)
class X25519EncMaterial : public EncMaterial {
  public:
    pgp_x25519_encrypted_t enc;

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};
#endif

#if defined(ENABLE_PQC)
class MlkemEcdhEncMaterial : public EncMaterial {
    pgp_pubkey_alg_t alg_;

  public:
    pgp_kyber_ecdh_encrypted_t enc;

    MlkemEcdhEncMaterial(pgp_pubkey_alg_t alg) : alg_(alg)
    {
    }

    bool parse(pgp_packet_body_t &pkt) noexcept override;
    void write(pgp_packet_body_t &pkt) const override;
};
#endif

} // namespace pgp

#endif // RNP_ENC_MATERIAL_HPP_
