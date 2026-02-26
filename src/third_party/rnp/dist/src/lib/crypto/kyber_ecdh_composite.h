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

#ifndef RNP_KYBER_ECDH_COMPOSITE_H_
#define RNP_KYBER_ECDH_COMPOSITE_H_

#include "config.h"
#include <rnp/rnp_def.h>
#include <vector>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include "crypto/kyber.h"
#include "crypto/kyber_common.h"
#include "crypto/ecdh.h"
#include "crypto/exdsa_ecdhkem.h"
#include <memory>

struct pgp_kyber_ecdh_key_t; /* forward declaration */

class pgp_kyber_ecdh_composite_key_t {
  public:
    virtual ~pgp_kyber_ecdh_composite_key_t() = 0;

    static rnp_result_t gen_keypair(rnp::RNG *            rng,
                                    pgp_kyber_ecdh_key_t *key,
                                    pgp_pubkey_alg_t      alg);

    static size_t            ecdh_curve_privkey_size(pgp_curve_t curve);
    static size_t            ecdh_curve_pubkey_size(pgp_curve_t curve);
    static size_t            ecdh_curve_ephemeral_size(pgp_curve_t curve);
    static size_t            ecdh_curve_keyshare_size(pgp_curve_t curve);
    static pgp_curve_t       pk_alg_to_curve_id(pgp_pubkey_alg_t pk_alg);
    static kyber_parameter_e pk_alg_to_kyber_id(pgp_pubkey_alg_t pk_alg);

    bool
    is_initialized() const
    {
        return is_initialized_;
    }

  protected:
    bool is_initialized_ = false;
    void initialized_or_throw() const;
};

typedef struct pgp_kyber_ecdh_encrypted_t {
    std::vector<uint8_t> composite_ciphertext;
    std::vector<uint8_t> wrapped_sesskey;

    static size_t
    composite_ciphertext_size(pgp_pubkey_alg_t pk_alg)
    {
        return kyber_ciphertext_size(
                 pgp_kyber_ecdh_composite_key_t::pk_alg_to_kyber_id(pk_alg)) +
               pgp_kyber_ecdh_composite_key_t::ecdh_curve_ephemeral_size(
                 pgp_kyber_ecdh_composite_key_t::pk_alg_to_curve_id(pk_alg));
    }
} pgp_kyber_ecdh_encrypted_t;

class pgp_kyber_ecdh_composite_private_key_t : public pgp_kyber_ecdh_composite_key_t {
  public:
    pgp_kyber_ecdh_composite_private_key_t(const uint8_t *  key_encoded,
                                           size_t           key_encoded_len,
                                           pgp_pubkey_alg_t pk_alg);
    pgp_kyber_ecdh_composite_private_key_t(std::vector<uint8_t> const &ecdh_key_encoded,
                                           std::vector<uint8_t> const &kyber_key_encoded,
                                           pgp_pubkey_alg_t            pk_alg);
    pgp_kyber_ecdh_composite_private_key_t(std::vector<uint8_t> const &key_encoded,
                                           pgp_pubkey_alg_t            pk_alg);
    pgp_kyber_ecdh_composite_private_key_t &operator=(
      const pgp_kyber_ecdh_composite_private_key_t &other);
    pgp_kyber_ecdh_composite_private_key_t(
      const pgp_kyber_ecdh_composite_private_key_t &other);
    pgp_kyber_ecdh_composite_private_key_t() = default;

    rnp_result_t decrypt(rnp::RNG *                        rng,
                         uint8_t *                         out,
                         size_t *                          out_len,
                         const pgp_kyber_ecdh_encrypted_t *enc) const;

    bool                 is_valid(rnp::RNG *rng) const;
    std::vector<uint8_t> get_encoded() const;

    pgp_pubkey_alg_t
    pk_alg() const
    {
        return pk_alg_;
    }

    void secure_clear();

    static size_t encoded_size(pgp_pubkey_alg_t pk_alg);

  private:
    void parse_component_keys(std::vector<uint8_t> key_encoded);

    pgp_pubkey_alg_t pk_alg_;

    /* kyber part */
    std::unique_ptr<pgp_kyber_private_key_t> kyber_key_;

    /* ecc part*/
    std::unique_ptr<ecdh_kem_private_key_t> ecdh_key_;
};

class pgp_kyber_ecdh_composite_public_key_t : public pgp_kyber_ecdh_composite_key_t {
  public:
    pgp_kyber_ecdh_composite_public_key_t(const uint8_t *  key_encoded,
                                          size_t           key_encoded_len,
                                          pgp_pubkey_alg_t pk_alg);
    pgp_kyber_ecdh_composite_public_key_t(std::vector<uint8_t> const &ecdh_key_encoded,
                                          std::vector<uint8_t> const &kyber_key_encoded,
                                          pgp_pubkey_alg_t            pk_alg);
    pgp_kyber_ecdh_composite_public_key_t(std::vector<uint8_t> const &key_encoded,
                                          pgp_pubkey_alg_t            pk_alg);
    pgp_kyber_ecdh_composite_public_key_t() = default;

    bool
    operator==(const pgp_kyber_ecdh_composite_public_key_t &rhs) const
    {
        return (pk_alg_ == rhs.pk_alg_) && (kyber_key_ == rhs.kyber_key_) &&
               (ecdh_key_ == rhs.ecdh_key_);
    }

    rnp_result_t encrypt(rnp::RNG *                  rng,
                         pgp_kyber_ecdh_encrypted_t *out,
                         const uint8_t *             in,
                         size_t                      in_len) const;

    bool                 is_valid(rnp::RNG *rng) const;
    std::vector<uint8_t> get_encoded() const;

    pgp_pubkey_alg_t
    pk_alg() const
    {
        return pk_alg_;
    }

    static size_t encoded_size(pgp_pubkey_alg_t pk_alg);

  private:
    void parse_component_keys(std::vector<uint8_t> key_encoded);

    pgp_pubkey_alg_t pk_alg_;

    /* kyber part */
    pgp_kyber_public_key_t kyber_key_;

    /* ecc part*/
    ecdh_kem_public_key_t ecdh_key_;
};

typedef struct pgp_kyber_ecdh_key_t {
    pgp_kyber_ecdh_composite_private_key_t priv;
    pgp_kyber_ecdh_composite_public_key_t  pub;
} pgp_kyber_ecdh_key_t;

rnp_result_t kyber_ecdh_validate_key(rnp::RNG *                  rng,
                                     const pgp_kyber_ecdh_key_t *key,
                                     bool                        secret);

#endif
