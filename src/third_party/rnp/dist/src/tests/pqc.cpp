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

#include "config.h"
#if defined(ENABLE_PQC)

#include "rnp_tests.h"
#include <array>
#include "crypto/dilithium.h"
#include "crypto/sphincsplus.h"
#include "crypto/kyber.h"

TEST_F(rnp_tests, test_kyber_key_function)
{
    kyber_parameter_e params[2] = {kyber_768, kyber_1024};
    for (kyber_parameter_e param : params) {
        auto public_and_private_key = kyber_generate_keypair(&global_ctx.rng, param);

        kyber_encap_result_t encap_res =
          public_and_private_key.first.encapsulate(&global_ctx.rng);

        std::vector<uint8_t> decrypted = public_and_private_key.second.decapsulate(
          &global_ctx.rng, encap_res.ciphertext.data(), encap_res.ciphertext.size());
        assert_int_equal(encap_res.symmetric_key.size(), decrypted.size());
        assert_memory_equal(
          encap_res.symmetric_key.data(), decrypted.data(), decrypted.size());
    }
}

TEST_F(rnp_tests, test_dilithium_key_function)
{
    dilithium_parameter_e params[2] = {dilithium_L3, dilithium_L5};
    for (dilithium_parameter_e param : params) {
        auto public_and_private_key = dilithium_generate_keypair(&global_ctx.rng, param);

        std::array<uint8_t, 5> msg{'H', 'e', 'l', 'l', 'o'};

        std::vector<uint8_t> signature =
          public_and_private_key.second.sign(&global_ctx.rng, msg.data(), msg.size());

        assert_true(public_and_private_key.first.verify_signature(
          msg.data(), msg.size(), signature.data(), signature.size()));
    }
}

TEST_F(rnp_tests, test_sphincsplus_key_function)
{
    sphincsplus_parameter_t params[] = {sphincsplus_simple_128s,
                                        sphincsplus_simple_128f,
                                        sphincsplus_simple_192s,
                                        sphincsplus_simple_192f,
                                        sphincsplus_simple_256s,
                                        sphincsplus_simple_256f};
    sphincsplus_hash_func_t hash_funcs[] = {sphincsplus_sha256, sphinscplus_shake256};

    for (sphincsplus_parameter_t param : params) {
        for (sphincsplus_hash_func_t hash_func : hash_funcs) {
            auto public_and_private_key =
              sphincsplus_generate_keypair(&global_ctx.rng, param, hash_func);

            std::array<uint8_t, 5> msg{'H', 'e', 'l', 'l', 'o'};

            pgp_sphincsplus_signature_t sig;
            assert_rnp_success(public_and_private_key.second.sign(
              &global_ctx.rng, &sig, msg.data(), msg.size()));

            assert_rnp_success(
              public_and_private_key.first.verify(&sig, msg.data(), msg.size()));
        }
    }
}

TEST_F(rnp_tests, test_dilithium_exdsa_direct)
{
    pgp_pubkey_alg_t algs[] = {PGP_PKA_DILITHIUM3_ED25519,
                               /* PGP_PKA_DILITHIUM5_ED448,*/ PGP_PKA_DILITHIUM3_P256,
                               PGP_PKA_DILITHIUM5_P384,
                               PGP_PKA_DILITHIUM3_BP256,
                               PGP_PKA_DILITHIUM5_BP384};

    for (size_t i = 0; i < ARRAY_SIZE(algs); i++) {
        uint8_t              message[64];
        const pgp_hash_alg_t hash_alg = PGP_HASH_SHA512;
        // Generate test data. Mainly to make valgrind not to complain about uninitialized data
        global_ctx.rng.get(message, sizeof(message));

        pgp_dilithium_exdsa_key_t       key;
        pgp_dilithium_exdsa_signature_t sig;

        assert_rnp_success(
          pgp_dilithium_exdsa_composite_key_t::gen_keypair(&global_ctx.rng, &key, algs[i]));

        assert_rnp_success(
          key.priv.sign(&global_ctx.rng, &sig, hash_alg, message, sizeof(message)));
        assert_rnp_success(key.pub.verify(&sig, hash_alg, message, sizeof(message)));

        // Fails because message won't verify
        message[0] = ~message[0];
        assert_rnp_failure(key.pub.verify(&sig, hash_alg, message, sizeof(message)));
        message[0] = ~message[0];

        // Fails because first sig won't verify
        sig.sig.data()[0] = ~sig.sig.data()[0];
        assert_rnp_failure(key.pub.verify(&sig, hash_alg, message, sizeof(message)));
        sig.sig.data()[0] = ~sig.sig.data()[0];

        // Fails because second sig won't verify
        sig.sig.data()[sig.sig.size() - 1] = ~sig.sig.data()[sig.sig.size() - 1];
        assert_rnp_failure(key.pub.verify(&sig, hash_alg, message, sizeof(message)));
        sig.sig.data()[sig.sig.size() - 1] = ~sig.sig.data()[sig.sig.size() - 1];
    }
}

#endif
