/*
 * Copyright (c) 2019-2020, [Ribose Inc](https://www.ribose.com).
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

#include <string.h>
#include <assert.h>
#include "defaults.h"
#include "utils.h"
#include "stream-ctx.h"

rnp_result_t
rnp_ctx_t::add_encryption_password(const std::string &password,
                                   pgp_hash_alg_t     halg,
                                   pgp_symm_alg_t     ealg,
                                   size_t             iterations)
{
    rnp_symmetric_pass_info_t info = {};

    info.s2k.usage = PGP_S2KU_ENCRYPTED_AND_HASHED;
    info.s2k.specifier = PGP_S2KS_ITERATED_AND_SALTED;
    info.s2k.hash_alg = halg;
    sec_ctx.rng.get(info.s2k.salt, sizeof(info.s2k.salt));
    if (!iterations) {
        iterations = sec_ctx.s2k_iterations(halg);
    }
    if (!iterations) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    info.s2k.iterations = pgp_s2k_encode_iterations(iterations);
    info.s2k_cipher = ealg;
    /* Note: we're relying on the fact that a longer-than-needed key length
     * here does not change the entire derived key (it just generates unused
     * extra bytes at the end). We derive a key of our maximum supported length,
     * which is a bit wasteful.
     *
     * This is done because we do not yet know what cipher this key will actually
     * end up being used with until later.
     *
     * An alternative would be to keep a list of actual passwords and s2k params,
     * and save the key derivation for later.
     */
    if (!pgp_s2k_derive_key(&info.s2k, password.c_str(), info.key.data(), info.key.size())) {
        return RNP_ERROR_GENERIC;
    }
    passwords.push_back(std::move(info));
    return RNP_SUCCESS;
}

#if defined(ENABLE_CRYPTO_REFRESH)
bool
rnp_ctx_t::pkeskv6_capable()
{
    for (auto *key : recipients) {
        if (key->version() < PGP_V6) {
            return false;
        }
    }
    return true;
}
#endif
