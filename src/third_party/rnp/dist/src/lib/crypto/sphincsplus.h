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

#ifndef SPHINCSPLUS_H_
#define SPHINCSPLUS_H_

#include "config.h"
#include <rnp/rnp_def.h>
#include <vector>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include <botan/sphincsplus.h>
#include <botan/pubkey.h>

struct pgp_sphincsplus_key_t;
struct pgp_sphincsplus_signature_t;

typedef enum { sphincsplus_sha256, sphinscplus_shake256 } sphincsplus_hash_func_t;
typedef enum : uint8_t {
    sphincsplus_simple_128s = 1,
    sphincsplus_simple_128f = 2,
    sphincsplus_simple_192s = 3,
    sphincsplus_simple_192f = 4,
    sphincsplus_simple_256s = 5,
    sphincsplus_simple_256f = 6
} sphincsplus_parameter_t;

typedef struct pgp_sphincsplus_signature_t {
    std::vector<uint8_t>    sig;
    sphincsplus_parameter_t param;
} pgp_sphincsplus_signature_t;

class pgp_sphincsplus_private_key_t {
  public:
    pgp_sphincsplus_private_key_t(const uint8_t *         key_encoded,
                                  size_t                  key_encoded_len,
                                  sphincsplus_parameter_t sphincs_param,
                                  sphincsplus_hash_func_t sphincs_hash_func);
    pgp_sphincsplus_private_key_t(std::vector<uint8_t> const &key_encoded,
                                  sphincsplus_parameter_t     sphincs_param,
                                  sphincsplus_hash_func_t     sphincs_hash_func);
    pgp_sphincsplus_private_key_t(std::vector<uint8_t> const &key_encoded,
                                  sphincsplus_parameter_t     param,
                                  pgp_pubkey_alg_t            alg);
    pgp_sphincsplus_private_key_t() = default;

    bool is_valid(rnp::RNG *rng) const;

    sphincsplus_parameter_t
    param() const
    {
        return sphincsplus_param_;
    }

    pgp_pubkey_alg_t
    alg() const
    {
        return pk_alg_;
    }

    sphincsplus_hash_func_t
    hash_func() const
    {
        return sphincsplus_hash_func_;
    }

    rnp_result_t sign(rnp::RNG *                   rng,
                      pgp_sphincsplus_signature_t *sig,
                      const uint8_t *              msg,
                      size_t                       msg_len) const;
    std::vector<uint8_t>
    get_encoded() const
    {
        return Botan::unlock(key_encoded_);
    };

    void
    secure_clear()
    {
        is_initialized_ = false;
        Botan::zap(key_encoded_);
    };

  private:
    Botan::SphincsPlus_PrivateKey botan_key() const;

    Botan::secure_vector<uint8_t> key_encoded_;
    pgp_pubkey_alg_t              pk_alg_;
    sphincsplus_parameter_t       sphincsplus_param_;
    sphincsplus_hash_func_t       sphincsplus_hash_func_;
    bool                          is_initialized_ = false;
};

class pgp_sphincsplus_public_key_t {
  public:
    pgp_sphincsplus_public_key_t(const uint8_t *         key_encoded,
                                 size_t                  key_encoded_len,
                                 sphincsplus_parameter_t sphincs_param,
                                 sphincsplus_hash_func_t sphincs_hash_func);
    pgp_sphincsplus_public_key_t(std::vector<uint8_t> const &key_encoded,
                                 sphincsplus_parameter_t     sphincs_param,
                                 sphincsplus_hash_func_t     sphincs_hash_func);
    pgp_sphincsplus_public_key_t(std::vector<uint8_t> const &key_encoded,
                                 sphincsplus_parameter_t     param,
                                 pgp_pubkey_alg_t            alg);
    pgp_sphincsplus_public_key_t() = default;

    bool
    operator==(const pgp_sphincsplus_public_key_t &rhs) const
    {
        return (sphincsplus_param_ == rhs.sphincsplus_param_) &&
               (sphincsplus_hash_func_ == rhs.sphincsplus_hash_func_) &&
               (key_encoded_ == rhs.key_encoded_);
    }

    rnp_result_t verify(const pgp_sphincsplus_signature_t *sig,
                        const uint8_t *                    msg,
                        size_t                             msg_len) const;

    bool is_valid(rnp::RNG *rng) const;

    bool validate_signature_hash_requirements(pgp_hash_alg_t hash_alg) const;

    sphincsplus_parameter_t
    param() const
    {
        return sphincsplus_param_;
    }

    pgp_pubkey_alg_t
    alg() const
    {
        return pk_alg_;
    }

    std::vector<uint8_t>
    get_encoded() const
    {
        return key_encoded_;
    };

  private:
    Botan::SphincsPlus_PublicKey botan_key() const;

    std::vector<uint8_t>    key_encoded_;
    pgp_pubkey_alg_t        pk_alg_;
    sphincsplus_parameter_t sphincsplus_param_;
    sphincsplus_hash_func_t sphincsplus_hash_func_;
    bool                    is_initialized_ = false;
};

std::pair<pgp_sphincsplus_public_key_t, pgp_sphincsplus_private_key_t>
sphincsplus_generate_keypair(rnp::RNG *              rng,
                             sphincsplus_parameter_t sphincs_param,
                             sphincsplus_hash_func_t sphincs_hash_func);

rnp_result_t pgp_sphincsplus_generate(rnp::RNG *              rng,
                                      pgp_sphincsplus_key_t * material,
                                      sphincsplus_parameter_t param,
                                      pgp_pubkey_alg_t        alg);

rnp_result_t sphincsplus_validate_key(rnp::RNG *                   rng,
                                      const pgp_sphincsplus_key_t *key,
                                      bool                         secret);

typedef struct pgp_sphincsplus_key_t {
    pgp_sphincsplus_public_key_t  pub;
    pgp_sphincsplus_private_key_t priv;
} pgp_sphincsplus_key_t;

size_t sphincsplus_privkey_size(sphincsplus_parameter_t param);
size_t sphincsplus_pubkey_size(sphincsplus_parameter_t param);
size_t sphincsplus_signature_size(sphincsplus_parameter_t param);

bool sphincsplus_hash_allowed(pgp_pubkey_alg_t        pk_alg,
                              sphincsplus_parameter_t sphincsplus_param,
                              pgp_hash_alg_t          hash_alg);

pgp_hash_alg_t sphincsplus_default_hash_alg(pgp_pubkey_alg_t        pk_alg,
                                            sphincsplus_parameter_t sphincsplus_param);

#endif