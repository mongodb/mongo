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

#ifndef RNP_RSA_H_
#define RNP_RSA_H_

#include <rnp/rnp_def.h>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include "crypto/mpi.hpp"
#include "mem.h"

namespace pgp {
namespace rsa {

class Signature {
  public:
    mpi s{};
};

class Encrypted {
  public:
    mpi m;
};

class Key {
  public:
    mpi n{};
    mpi e{};
    /* secret mpis */
    mpi d{};
    mpi p{};
    mpi q{};
    mpi u{};

    void
    clear_secret()
    {
        d.forget();
        p.forget();
        q.forget();
        u.forget();
    }

    ~Key()
    {
        clear_secret();
    }

    rnp_result_t validate(rnp::RNG &rng, bool secret) const noexcept;

    rnp_result_t generate(rnp::RNG &rng, size_t numbits) noexcept;

    rnp_result_t encrypt_pkcs1(rnp::RNG &               rng,
                               Encrypted &              out,
                               const rnp::secure_bytes &in) const noexcept;

    rnp_result_t decrypt_pkcs1(rnp::RNG &         rng,
                               rnp::secure_bytes &out,
                               const Encrypted &  in) const noexcept;

    rnp_result_t verify_pkcs1(const Signature &        sig,
                              pgp_hash_alg_t           hash_alg,
                              const rnp::secure_bytes &hash) const noexcept;

    rnp_result_t sign_pkcs1(rnp::RNG &               rng,
                            Signature &              sig,
                            pgp_hash_alg_t           hash_alg,
                            const rnp::secure_bytes &hash) const noexcept;
};

} // namespace rsa
} // namespace pgp

#endif
