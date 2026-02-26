/*-
 * Copyright (c) 2017-2022 Ribose Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_SM2_H_
#define RNP_SM2_H_

#include "config.h"
#include "ec.h"

namespace rnp {
class Hash;
} // namespace rnp

namespace pgp {
namespace sm2 {
class Encrypted {
  public:
    mpi m;
};

#if defined(ENABLE_SM2)
rnp_result_t validate_key(rnp::RNG &rng, const ec::Key &key, bool secret);

/**
 * Compute the SM2 "ZA" field, and add it to the hash object
 *
 * If ident_field is null, uses the default value
 */
rnp_result_t compute_za(const pgp::ec::Key &key,
                        rnp::Hash &         hash,
                        const char *        ident_field = NULL);

rnp_result_t sign(rnp::RNG &               rng,
                  ec::Signature &          sig,
                  pgp_hash_alg_t           hash_alg,
                  const rnp::secure_bytes &hash,
                  const pgp::ec::Key &     key);

rnp_result_t verify(const ec::Signature &    sig,
                    pgp_hash_alg_t           hash_alg,
                    const rnp::secure_bytes &hash,
                    const ec::Key &          key);

rnp_result_t encrypt(rnp::RNG &               rng,
                     Encrypted &              out,
                     const rnp::secure_bytes &in,
                     pgp_hash_alg_t           hash_algo,
                     const ec::Key &          key);

rnp_result_t decrypt(rnp::secure_bytes &out, const Encrypted &in, const ec::Key &key);
#endif // defined(ENABLE_SM2)
} // namespace sm2
} // namespace pgp

#endif // SM2_H_
