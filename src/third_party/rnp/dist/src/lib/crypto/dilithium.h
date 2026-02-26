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

#ifndef DILITHIUM_H_
#define DILITHIUM_H_

#include "config.h"
#include <rnp/rnp_def.h>
#include <vector>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include <botan/dilithium.h>
#include <botan/pubkey.h>

enum dilithium_parameter_e { dilithium_L3, dilithium_L5 };

class pgp_dilithium_private_key_t {
  public:
    pgp_dilithium_private_key_t(const uint8_t *       key_encoded,
                                size_t                key_encoded_len,
                                dilithium_parameter_e param);
    pgp_dilithium_private_key_t(std::vector<uint8_t> const &key_encoded,
                                dilithium_parameter_e       param);
    pgp_dilithium_private_key_t() = default;

    bool is_valid(rnp::RNG *rng) const;

    dilithium_parameter_e
    param() const
    {
        return dilithium_param_;
    }

    std::vector<uint8_t> sign(rnp::RNG *rng, const uint8_t *msg, size_t msg_len) const;
    std::vector<uint8_t>
    get_encoded() const
    {
        return Botan::unlock(key_encoded_);
    };

  private:
    Botan::Dilithium_PrivateKey botan_key() const;

    Botan::secure_vector<uint8_t> key_encoded_;
    dilithium_parameter_e         dilithium_param_;
    bool                          is_initialized_ = false;
};

class pgp_dilithium_public_key_t {
  public:
    pgp_dilithium_public_key_t(const uint8_t *       key_encoded,
                               size_t                key_encoded_len,
                               dilithium_parameter_e mode);
    pgp_dilithium_public_key_t(std::vector<uint8_t> const &key_encoded,
                               dilithium_parameter_e       mode);
    pgp_dilithium_public_key_t() = default;

    bool
    operator==(const pgp_dilithium_public_key_t &rhs) const
    {
        return (dilithium_param_ == rhs.dilithium_param_) &&
               (key_encoded_ == rhs.key_encoded_);
    }

    bool verify_signature(const uint8_t *msg,
                          size_t         msg_len,
                          const uint8_t *signature,
                          size_t         signature_len) const;

    bool is_valid(rnp::RNG *rng) const;

    std::vector<uint8_t>
    get_encoded() const
    {
        return key_encoded_;
    };

  private:
    Botan::Dilithium_PublicKey botan_key() const;

    std::vector<uint8_t>  key_encoded_;
    dilithium_parameter_e dilithium_param_;
    bool                  is_initialized_ = false;
};

std::pair<pgp_dilithium_public_key_t, pgp_dilithium_private_key_t> dilithium_generate_keypair(
  rnp::RNG *rng, dilithium_parameter_e dilithium_param);

bool dilithium_hash_allowed(pgp_hash_alg_t hash_alg);

pgp_hash_alg_t dilithium_default_hash_alg();

#endif
