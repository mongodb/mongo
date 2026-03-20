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

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)

#include "rnp_tests.h"
#include "crypto/exdsa_ecdhkem.h"
#include "crypto/bn.h"

TEST_F(rnp_tests, test_ecdh_kem)
{
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> symmetric_key;
    std::vector<uint8_t> symmetric_key2;
    ecdh_kem_key_t       key_pair;
    pgp_curve_t          curve_list[] = {PGP_CURVE_NIST_P_256,
                                PGP_CURVE_NIST_P_384,
                                PGP_CURVE_NIST_P_521,
                                PGP_CURVE_BP256,
                                PGP_CURVE_BP384,
                                PGP_CURVE_BP512,
                                PGP_CURVE_25519};

    for (auto curve : curve_list) {
        /* keygen */
        assert_rnp_success(
          ec_key_t::generate_ecdh_kem_key_pair(&global_ctx.rng, &key_pair, curve));

        /* kem encaps / decaps */
        assert_rnp_success(
          key_pair.pub.encapsulate(&global_ctx.rng, ciphertext, symmetric_key));
        assert_rnp_success(
          key_pair.priv.decapsulate(&global_ctx.rng, ciphertext, symmetric_key2));

        /* both parties should have the same key share */
        assert_int_equal(symmetric_key.size(), symmetric_key2.size());
        assert_memory_equal(symmetric_key.data(), symmetric_key2.data(), symmetric_key.size());

        /* test invalid ciphertext */
        ciphertext.data()[4] += 1;
        if (curve != PGP_CURVE_25519) { // Curve25519 accepts any 32-byte array
            assert_throw(
              key_pair.priv.decapsulate(&global_ctx.rng, ciphertext, symmetric_key));
        }
    }
}

TEST_F(rnp_tests, test_exdsa)
{
    pgp_hash_alg_t       hash_alg = PGP_HASH_SHA256;
    std::vector<uint8_t> msg(32);
    exdsa_key_t          key_pair;
    pgp_curve_t          curve_list[] = {PGP_CURVE_NIST_P_256,
                                PGP_CURVE_NIST_P_384,
                                PGP_CURVE_NIST_P_521,
                                PGP_CURVE_BP256,
                                PGP_CURVE_BP384,
                                PGP_CURVE_BP512,
                                PGP_CURVE_ED25519};
    // pgp_curve_t curve_list[] = {PGP_CURVE_ED25519};

    for (auto curve : curve_list) {
        /* keygen */
        assert_rnp_success(
          ec_key_t::generate_exdsa_key_pair(&global_ctx.rng, &key_pair, curve));

        /* sign and verify */
        std::vector<uint8_t> sig;
        assert_rnp_success(
          key_pair.priv.sign(&global_ctx.rng, sig, msg.data(), msg.size(), hash_alg));
        assert_rnp_success(key_pair.pub.verify(sig, msg.data(), msg.size(), hash_alg));

        /* test invalid msg / hash */
        msg.data()[4] -= 1;
        assert_rnp_failure(key_pair.pub.verify(sig, msg.data(), msg.size(), hash_alg));

        /* test invalid sig */
        msg.data()[4] += 1;
        sig.data()[4] -= 1;
        assert_rnp_failure(key_pair.pub.verify(sig, msg.data(), msg.size(), hash_alg));
    }
}

#endif
