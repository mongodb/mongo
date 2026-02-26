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

#include "ed25519.h"
#include "logging.h"
#include "utils.h"

#include <botan/pubkey.h>
#include <botan/ed25519.h>
#include <cassert>

rnp_result_t
generate_ed25519_native(rnp::RNG *            rng,
                        std::vector<uint8_t> &privkey,
                        std::vector<uint8_t> &pubkey)
{
    Botan::Ed25519_PrivateKey private_key(*(rng->obj()));
    const size_t              key_len = 32;
    auto                      priv_pub = Botan::unlock(private_key.raw_private_key_bits());
    assert(priv_pub.size() == 2 * key_len);
    privkey = std::vector<uint8_t>(priv_pub.begin(), priv_pub.begin() + key_len);
    pubkey = std::vector<uint8_t>(priv_pub.begin() + key_len, priv_pub.end());

    return RNP_SUCCESS;
}

rnp_result_t
ed25519_sign_native(rnp::RNG *                  rng,
                    std::vector<uint8_t> &      sig_out,
                    const std::vector<uint8_t> &key,
                    const uint8_t *             hash,
                    size_t                      hash_len)
{
    Botan::Ed25519_PrivateKey priv_key(Botan::secure_vector<uint8_t>(key.begin(), key.end()));
    auto                      signer = Botan::PK_Signer(priv_key, *(rng->obj()), "Pure");
    sig_out = signer.sign_message(hash, hash_len, *(rng->obj()));

    return RNP_SUCCESS;
}

rnp_result_t
ed25519_verify_native(const std::vector<uint8_t> &sig,
                      const std::vector<uint8_t> &key,
                      const uint8_t *             hash,
                      size_t                      hash_len)
{
    Botan::Ed25519_PublicKey pub_key(key);
    auto                     verifier = Botan::PK_Verifier(pub_key, "Pure");
    if (verifier.verify_message(hash, hash_len, sig.data(), sig.size())) {
        return RNP_SUCCESS;
    }
    return RNP_ERROR_VERIFICATION_FAILED;
}

rnp_result_t
ed25519_validate_key_native(rnp::RNG *rng, const pgp_ed25519_key_t *key, bool secret)
{
    Botan::Ed25519_PublicKey pub_key(key->pub);
    if (!pub_key.check_key(*(rng->obj()), false)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (secret) {
        Botan::Ed25519_PrivateKey priv_key(
          Botan::secure_vector<uint8_t>(key->priv.begin(), key->priv.end()));
        if (!priv_key.check_key(*(rng->obj()), false)) {
            return RNP_ERROR_SIGNING_FAILED;
        }
    }

    return RNP_SUCCESS;
}
