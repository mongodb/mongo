/*-
 * Copyright (c) 2017-2024 Ribose Inc.
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

#ifndef ECDSA_H_
#define ECDSA_H_

#include "crypto/ec.h"

namespace pgp {
namespace ecdsa {
rnp_result_t validate_key(rnp::RNG &rng, const ec::Key &key, bool secret);

rnp_result_t sign(rnp::RNG &               rng,
                  ec::Signature &          sig,
                  pgp_hash_alg_t           hash_alg,
                  const rnp::secure_bytes &hash,
                  const ec::Key &          key);

rnp_result_t verify(const ec::Signature &    sig,
                    pgp_hash_alg_t           hash_alg,
                    const rnp::secure_bytes &hash,
                    const ec::Key &          key);

const char *padding_str_for(pgp_hash_alg_t hash_alg);

/*
 * @brief   Returns hash which should be used with the curve
 *
 * @param   curve Curve ID
 *
 * @returns  Either ID of the hash algorithm, or PGP_HASH_UNKNOWN
 *           if not found
 */
pgp_hash_alg_t get_min_hash(pgp_curve_t curve);

} // namespace ecdsa
} // namespace pgp

#endif // ECDSA_H_
