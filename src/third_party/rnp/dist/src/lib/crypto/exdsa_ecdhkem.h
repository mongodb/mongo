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

#ifndef ECDH_KEM_H_
#define ECDH_KEM_H_

#include "config.h"
#include <rnp/rnp_def.h>
#include <vector>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include <memory>
#include "botan/secmem.h"
#include <botan/pubkey.h>
#include <botan/ecdsa.h>
#include <botan/ecdh.h>
#include <botan/ed25519.h>
#include <botan/curve25519.h>

struct ecdh_kem_key_t; /* forward declaration */
struct exdsa_key_t;    /* forward declaration */

class ec_key_t {
  public:
    virtual ~ec_key_t() = 0;
    ec_key_t(pgp_curve_t curve);
    ec_key_t() = default;

    static rnp_result_t generate_ecdh_kem_key_pair(rnp::RNG *      rng,
                                                   ecdh_kem_key_t *out,
                                                   pgp_curve_t     curve);
    static rnp_result_t generate_exdsa_key_pair(rnp::RNG *   rng,
                                                exdsa_key_t *out,
                                                pgp_curve_t  curve);

    pgp_curve_t
    get_curve() const
    {
        return curve_;
    }

  protected:
    pgp_curve_t curve_;
};

class ecdh_kem_public_key_t : public ec_key_t {
  public:
    ecdh_kem_public_key_t(uint8_t *key_buf, size_t key_buf_len, pgp_curve_t curve);
    ecdh_kem_public_key_t(std::vector<uint8_t> key_buf, pgp_curve_t curve);
    ecdh_kem_public_key_t() = default;

    bool
    operator==(const ecdh_kem_public_key_t &rhs) const
    {
        return (curve_ == rhs.curve_) && (key_ == rhs.key_);
    }

    bool is_valid(rnp::RNG *rng) const;

    std::vector<uint8_t>
    get_encoded() const
    {
        return key_;
    }

    rnp_result_t encapsulate(rnp::RNG *            rng,
                             std::vector<uint8_t> &ciphertext,
                             std::vector<uint8_t> &symmetric_key) const;

  private:
    Botan::ECDH_PublicKey       botan_key_ecdh(rnp::RNG *rng) const;
    Botan::Curve25519_PublicKey botan_key_x25519() const;

    std::vector<uint8_t> key_;
};

class ecdh_kem_private_key_t : public ec_key_t {
  public:
    ecdh_kem_private_key_t(uint8_t *key_buf, size_t key_buf_len, pgp_curve_t curve);
    ecdh_kem_private_key_t(std::vector<uint8_t> key_buf, pgp_curve_t curve);
    ecdh_kem_private_key_t() = default;

    bool is_valid(rnp::RNG *rng) const;

    std::vector<uint8_t>
    get_encoded() const
    {
        return Botan::unlock(key_);
    }

    std::vector<uint8_t> get_pubkey_encoded(rnp::RNG *rng) const;

    rnp_result_t decapsulate(rnp::RNG *                  rng,
                             const std::vector<uint8_t> &ciphertext,
                             std::vector<uint8_t> &      plaintext);

  private:
    Botan::ECDH_PrivateKey       botan_key_ecdh(rnp::RNG *rng) const;
    Botan::Curve25519_PrivateKey botan_key_x25519() const;

    Botan::secure_vector<uint8_t> key_;
};

typedef struct ecdh_kem_key_t {
    ecdh_kem_private_key_t priv;
    ecdh_kem_public_key_t  pub;
} ecdh_kem_key_t;

class exdsa_public_key_t : public ec_key_t {
  public:
    exdsa_public_key_t(uint8_t *key_buf, size_t key_buf_len, pgp_curve_t curve);
    exdsa_public_key_t(std::vector<uint8_t> key_buf, pgp_curve_t curve);
    exdsa_public_key_t() = default;

    bool
    operator==(const exdsa_public_key_t &rhs) const
    {
        return (curve_ == rhs.curve_) && (key_ == rhs.key_);
    }

    bool is_valid(rnp::RNG *rng) const;

    std::vector<uint8_t>
    get_encoded() const
    {
        return key_;
    }

    rnp_result_t verify(const std::vector<uint8_t> &sig,
                        const uint8_t *             hash,
                        size_t                      hash_len,
                        pgp_hash_alg_t              hash_alg) const;

  private:
    Botan::ECDSA_PublicKey botan_key() const;

    std::vector<uint8_t> key_;
};

class exdsa_private_key_t : public ec_key_t {
  public:
    exdsa_private_key_t(uint8_t *key_buf, size_t key_buf_len, pgp_curve_t curve);
    exdsa_private_key_t(std::vector<uint8_t> key_buf, pgp_curve_t curve);
    exdsa_private_key_t() = default;

    bool is_valid(rnp::RNG *rng) const;

    std::vector<uint8_t>
    get_encoded() const
    {
        return Botan::unlock(key_);
    }

    rnp_result_t sign(rnp::RNG *            rng,
                      std::vector<uint8_t> &sig_out,
                      const uint8_t *       hash,
                      size_t                hash_len,
                      pgp_hash_alg_t        hash_alg) const;

  private:
    Botan::ECDSA_PrivateKey botan_key(rnp::RNG *rng) const;

    Botan::secure_vector<uint8_t> key_;
};

typedef struct exdsa_key_t {
    exdsa_private_key_t priv;
    exdsa_public_key_t  pub;
} exdsa_key_t;

#endif
