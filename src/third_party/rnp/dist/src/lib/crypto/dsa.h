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

#ifndef RNP_DSA_H_
#define RNP_DSA_H_

#include <rnp/rnp_def.h>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include "crypto/mpi.hpp"
#include "crypto/mem.h"

namespace pgp {
namespace dsa {

class Signature {
  public:
    mpi r{};
    mpi s{};
};

class Key {
  public:
    mpi p{};
    mpi q{};
    mpi g{};
    mpi y{};
    /* secret mpi */
    mpi x{};

    void
    clear_secret()
    {
        x.forget();
    }

    ~Key()
    {
        clear_secret();
    }

    /**
     * @brief Checks DSA key fields for validity
     *
     * @param rng initialized PRNG
     * @param secret flag which tells whether key has populated secret fields
     *
     * @return RNP_SUCCESS if key is valid or error code otherwise
     */
    rnp_result_t validate(rnp::RNG &rng, bool secret) const noexcept;

    /*
     * @brief   Performs DSA signing
     *
     * @param   rng       initialized PRNG
     * @param   sig[out]  created signature
     * @param   hash      hash to sign
     * @param   hash_len  length of `hash`
     *
     * @returns RNP_SUCCESS
     *          RNP_ERROR_BAD_PARAMETERS wrong input provided
     *          RNP_ERROR_SIGNING_FAILED internal error
     */
    rnp_result_t sign(rnp::RNG &rng, Signature &sig, const rnp::secure_bytes &hash) const;

    /*
     * @brief   Performs DSA verification
     *
     * @param   hash      hash to verify
     * @param   hash_len  length of `hash`
     * @param   sig       signature to be verified
     *
     * @returns RNP_SUCCESS
     *          RNP_ERROR_BAD_PARAMETERS wrong input provided
     *          RNP_ERROR_GENERIC internal error
     *          RNP_ERROR_SIGNATURE_INVALID signature is invalid
     */
    rnp_result_t verify(const Signature &sig, const rnp::secure_bytes &hash) const;

    /*
     * @brief   Performs DSA key generation
     *
     * @param   rng          initialized PRNG
     * @param   keylen       length of the key, in bits
     * @param   qbits        subgroup size in bits
     *
     * @returns RNP_SUCCESS
     *          RNP_ERROR_BAD_PARAMETERS wrong input provided
     *          RNP_ERROR_OUT_OF_MEMORY memory allocation failed
     *          RNP_ERROR_GENERIC internal error
     */
    rnp_result_t generate(rnp::RNG &rng, size_t keylen, size_t qbits);

    /*
     * @brief   Returns minimally sized hash which will work
     *          with the DSA subgroup.
     *
     * @param   qsize subgroup order
     *
     * @returns  Either ID of the hash algorithm, or PGP_HASH_UNKNOWN
     *           if not found
     */
    static pgp_hash_alg_t get_min_hash(size_t qsize);

    /*
     * @brief   Helps to determine subgroup size by size of p
     *          In order not to confuse users, we use less complicated
     *          approach than suggested by FIPS-186, which is:
     *            p=1024  => q=160
     *            p<2048  => q=224
     *            p<=3072 => q=256
     *          So we don't generate (2048, 224) pair
     *
     * @return  Size of `q' or 0 in case `psize' is not in <1024,3072> range
     */
    static size_t choose_qsize(size_t psize);
};

} // namespace dsa
} // namespace pgp

#endif
