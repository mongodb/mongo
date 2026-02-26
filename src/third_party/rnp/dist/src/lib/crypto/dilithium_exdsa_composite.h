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

#ifndef DILITHIUM_EXDSA_COMPOSITE_H_
#define DILITHIUM_EXDSA_COMPOSITE_H_

#include "config.h"
#include <rnp/rnp_def.h>
#include <vector>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include "crypto/dilithium.h"
#include "crypto/dilithium_common.h"
#include "crypto/exdsa_ecdhkem.h"
#include <memory>

struct pgp_dilithium_exdsa_key_t; /* forward declaration */

class pgp_dilithium_exdsa_composite_key_t {
  public:
    virtual ~pgp_dilithium_exdsa_composite_key_t() = 0;

    static rnp_result_t gen_keypair(rnp::RNG *                 rng,
                                    pgp_dilithium_exdsa_key_t *key,
                                    pgp_pubkey_alg_t           alg);

    static size_t                exdsa_curve_privkey_size(pgp_curve_t curve);
    static size_t                exdsa_curve_pubkey_size(pgp_curve_t curve);
    static size_t                exdsa_curve_signature_size(pgp_curve_t curve);
    static pgp_curve_t           pk_alg_to_curve_id(pgp_pubkey_alg_t pk_alg);
    static dilithium_parameter_e pk_alg_to_dilithium_id(pgp_pubkey_alg_t pk_alg);

    bool
    is_initialized() const
    {
        return is_initialized_;
    }

  protected:
    bool is_initialized_ = false;
    void initialized_or_throw() const;
};

typedef struct pgp_dilithium_exdsa_signature_t {
    std::vector<uint8_t> sig;

    static size_t
    composite_signature_size(pgp_pubkey_alg_t pk_alg)
    {
        return dilithium_signature_size(
                 pgp_dilithium_exdsa_composite_key_t::pk_alg_to_dilithium_id(pk_alg)) +
               pgp_dilithium_exdsa_composite_key_t::exdsa_curve_signature_size(
                 pgp_dilithium_exdsa_composite_key_t::pk_alg_to_curve_id(pk_alg));
    }
} pgp_dilithium_exdsa_signature_t;

class pgp_dilithium_exdsa_composite_private_key_t
    : public pgp_dilithium_exdsa_composite_key_t {
  public:
    pgp_dilithium_exdsa_composite_private_key_t(const uint8_t *  key_encoded,
                                                size_t           key_encoded_len,
                                                pgp_pubkey_alg_t pk_alg);
    pgp_dilithium_exdsa_composite_private_key_t(
      std::vector<uint8_t> const &exdsa_key_encoded,
      std::vector<uint8_t> const &dilithium_key_encoded,
      pgp_pubkey_alg_t            pk_alg);
    pgp_dilithium_exdsa_composite_private_key_t(std::vector<uint8_t> const &key_encoded,
                                                pgp_pubkey_alg_t            pk_alg);
    pgp_dilithium_exdsa_composite_private_key_t &operator=(
      const pgp_dilithium_exdsa_composite_private_key_t &other);
    pgp_dilithium_exdsa_composite_private_key_t(
      const pgp_dilithium_exdsa_composite_private_key_t &other);
    pgp_dilithium_exdsa_composite_private_key_t() = default;

    rnp_result_t sign(rnp::RNG *                       rng,
                      pgp_dilithium_exdsa_signature_t *sig,
                      pgp_hash_alg_t                   hash_alg,
                      const uint8_t *                  msg,
                      size_t                           msg_len) const;

    std::vector<uint8_t> get_encoded() const;

    pgp_pubkey_alg_t
    pk_alg() const
    {
        return pk_alg_;
    }

    bool is_valid(rnp::RNG *rng) const;
    void secure_clear();

    static size_t encoded_size(pgp_pubkey_alg_t pk_alg);

  private:
    void parse_component_keys(std::vector<uint8_t> key_encoded);

    pgp_pubkey_alg_t pk_alg_;

    /* dilithium part */
    std::unique_ptr<pgp_dilithium_private_key_t> dilithium_key_;

    /* ecc part*/
    std::unique_ptr<exdsa_private_key_t> exdsa_key_;
};

class pgp_dilithium_exdsa_composite_public_key_t : public pgp_dilithium_exdsa_composite_key_t {
  public:
    pgp_dilithium_exdsa_composite_public_key_t(const uint8_t *  key_encoded,
                                               size_t           key_encoded_len,
                                               pgp_pubkey_alg_t pk_alg);
    pgp_dilithium_exdsa_composite_public_key_t(
      std::vector<uint8_t> const &exdsa_key_encoded,
      std::vector<uint8_t> const &dilithium_key_encoded,
      pgp_pubkey_alg_t            pk_alg);
    pgp_dilithium_exdsa_composite_public_key_t(std::vector<uint8_t> const &key_encoded,
                                               pgp_pubkey_alg_t            pk_alg);
    pgp_dilithium_exdsa_composite_public_key_t() = default;

    bool
    operator==(const pgp_dilithium_exdsa_composite_public_key_t &rhs) const
    {
        return (pk_alg_ == rhs.pk_alg_) && (dilithium_key_ == rhs.dilithium_key_) &&
               (exdsa_key_ == rhs.exdsa_key_);
    }

    rnp_result_t verify(const pgp_dilithium_exdsa_signature_t *sig,
                        pgp_hash_alg_t                         hash_alg,
                        const uint8_t *                        hash,
                        size_t                                 hash_len) const;

    std::vector<uint8_t> get_encoded() const;

    pgp_pubkey_alg_t
    pk_alg() const
    {
        return pk_alg_;
    }

    bool          is_valid(rnp::RNG *rng) const;
    static size_t encoded_size(pgp_pubkey_alg_t pk_alg);

  private:
    void parse_component_keys(std::vector<uint8_t> key_encoded);

    pgp_pubkey_alg_t pk_alg_;

    /* dilithium part */
    pgp_dilithium_public_key_t dilithium_key_;

    /* ecc part*/
    exdsa_public_key_t exdsa_key_;
};

typedef struct pgp_dilithium_exdsa_key_t {
    pgp_dilithium_exdsa_composite_private_key_t priv;
    pgp_dilithium_exdsa_composite_public_key_t  pub;
} pgp_dilithium_exdsa_key_t;

rnp_result_t dilithium_exdsa_validate_key(rnp::RNG *                       rng,
                                          const pgp_dilithium_exdsa_key_t *key,
                                          bool                             secret);

#endif
