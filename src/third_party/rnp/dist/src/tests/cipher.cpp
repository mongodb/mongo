/*
 * Copyright (c) 2017-2022 [Ribose Inc](https://www.ribose.com).
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

#include <crypto/common.h>
#include "rnp.h"
#include <librepgp/stream-packet.h>
#include <librepgp/stream-key.h>

#include "rnp_tests.h"
#include "support.h"
#include "fingerprint.hpp"
#include "keygen.hpp"

TEST_F(rnp_tests, hash_test_success)
{
    uint8_t hash_output[PGP_MAX_HASH_SIZE];

    const pgp_hash_alg_t hash_algs[] = {PGP_HASH_MD5,
                                        PGP_HASH_SHA1,
                                        PGP_HASH_SHA256,
                                        PGP_HASH_SHA384,
                                        PGP_HASH_SHA512,
                                        PGP_HASH_SHA224,
                                        PGP_HASH_SM3,
                                        PGP_HASH_SHA3_256,
                                        PGP_HASH_SHA3_512,
                                        PGP_HASH_UNKNOWN};

    const uint8_t test_input[3] = {'a', 'b', 'c'};
    const char *  hash_alg_expected_outputs[] = {
      "900150983CD24FB0D6963F7D28E17F72",
      "A9993E364706816ABA3E25717850C26C9CD0D89D",
      "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
      "CB00753F45A35E8BB5A03D699AC65007272C32AB0EDED1631A8B605A43FF5BED8086072BA1"
      "E7CC2358BAECA"
      "134C825A7",
      "DDAF35A193617ABACC417349AE20413112E6FA4E89A97EA20A9EEEE64B55D39A2192992A27"
      "4FC1A836BA3C2"
      "3A3FEEBBD454D4423643CE80E2A9AC94FA54CA49F",
      "23097D223405D8228642A477BDA255B32AADBCE4BDA0B3F7E36C9DA7",
      "66C7F0F462EEEDD9D1F2D46BDC10E4E24167C4875CF2F7A2297DA02B8F4BA8E0",
      "3A985DA74FE225B2045C172D6BD390BD855F086E3E9D525B46BFE24511431532",
      ("B751850B1A57168A5693CD924B6B096E08F621827444F70D884F5D0240D2712E1"
       "0E116E9192AF3C91A7EC57647E3934057340B4CF408D5A56592F8274EEC53F0")};

    for (int i = 0; hash_algs[i] != PGP_HASH_UNKNOWN; ++i) {
#if !defined(ENABLE_SM2)
        if (hash_algs[i] == PGP_HASH_SM3) {
            assert_throw({ auto hash = rnp::Hash::create(hash_algs[i]); });
            size_t hash_size = rnp::Hash::size(hash_algs[i]);
            assert_int_equal(hash_size * 2, strlen(hash_alg_expected_outputs[i]));
            continue;
        }
#endif
        auto   hash = rnp::Hash::create(hash_algs[i]);
        size_t hash_size = rnp::Hash::size(hash_algs[i]);
        assert_int_equal(hash_size * 2, strlen(hash_alg_expected_outputs[i]));

        hash->add(test_input, 1);
        hash->add(test_input + 1, sizeof(test_input) - 1);
        hash->finish(hash_output);

        assert_true(bin_eq_hex(hash_output, hash_size, hash_alg_expected_outputs[i]));
    }
}

TEST_F(rnp_tests, cipher_test_success)
{
    const uint8_t  key[16] = {0};
    uint8_t        iv[16];
    pgp_symm_alg_t alg = PGP_SA_AES_128;
    pgp_crypt_t    crypt;

    uint8_t cfb_data[20] = {0};
    memset(iv, 0x42, sizeof(iv));

    assert_int_equal(1, pgp_cipher_cfb_start(&crypt, alg, key, iv));

    assert_int_equal(0, pgp_cipher_cfb_encrypt(&crypt, cfb_data, cfb_data, sizeof(cfb_data)));

    assert_true(
      bin_eq_hex(cfb_data, sizeof(cfb_data), "BFDAA57CB812189713A950AD9947887983021617"));
    assert_int_equal(0, pgp_cipher_cfb_finish(&crypt));

    assert_int_equal(1, pgp_cipher_cfb_start(&crypt, alg, key, iv));
    assert_int_equal(0, pgp_cipher_cfb_decrypt(&crypt, cfb_data, cfb_data, sizeof(cfb_data)));
    assert_true(
      bin_eq_hex(cfb_data, sizeof(cfb_data), "0000000000000000000000000000000000000000"));
    assert_int_equal(0, pgp_cipher_cfb_finish(&crypt));
}

TEST_F(rnp_tests, pkcs1_rsa_test_success)
{
    rnp::secure_bytes ptext({'a', 'b', 'c'});
    rnp::secure_bytes dec;

    rnp::KeygenParams keygen(PGP_PKA_RSA, global_ctx);
    auto &            rsa = dynamic_cast<pgp::RSAKeyParams &>(keygen.key_params());
    rsa.set_bits(1024);

    pgp_key_pkt_t seckey;
    assert_true(keygen.generate(seckey, true));

    pgp::RSAEncMaterial enc;
    pgp::EGEncMaterial  enc2;
    assert_rnp_failure(seckey.material->encrypt(global_ctx, enc2, ptext));
    assert_rnp_success(seckey.material->encrypt(global_ctx, enc, ptext));
    assert_int_equal(enc.enc.m.size(), 1024 / 8);

    assert_rnp_failure(seckey.material->decrypt(global_ctx, dec, enc2));
    assert_true(dec.empty());
    assert_rnp_success(seckey.material->decrypt(global_ctx, dec, enc));
    assert_int_equal(dec.size(), 3);
    assert_true(bin_eq_hex(dec.data(), 3, "616263"));

    /* Try signing */
    assert_true(keygen.generate(seckey, true));

    rnp::secure_bytes hash(32);
    global_ctx.rng.get(hash.data(), hash.size());
    pgp::RSASigMaterial sig(PGP_HASH_SHA256);
    pgp::DSASigMaterial sig2(PGP_HASH_SHA256);

    assert_rnp_failure(seckey.material->sign(global_ctx, sig2, hash));
    assert_rnp_failure(seckey.material->verify(global_ctx, sig2, hash));
    assert_rnp_success(seckey.material->sign(global_ctx, sig, hash));
    assert_rnp_success(seckey.material->verify(global_ctx, sig, hash));

    // cut one byte off hash -> invalid sig
    rnp::secure_bytes hash_cut(hash.begin(), hash.end() - 1);
    assert_rnp_failure(seckey.material->verify(global_ctx, sig, hash_cut));

    // modify sig
    sig.sig.s[0] ^= 0xff;
    assert_rnp_failure(seckey.material->verify(global_ctx, sig, hash));
}

TEST_F(rnp_tests, pkcs1_rsa_test_sign_enc_only)
{
    rnp::KeygenParams keygen(PGP_PKA_RSA_SIGN_ONLY, global_ctx);
    auto &            rsa = dynamic_cast<pgp::RSAKeyParams &>(keygen.key_params());
    rsa.set_bits(1024);

    pgp_key_pkt_t seckey;
    assert_false(keygen.generate(seckey, true));

    rnp::KeygenParams keygen2(PGP_PKA_RSA_ENCRYPT_ONLY, global_ctx);
    auto &            rsa2 = dynamic_cast<pgp::RSAKeyParams &>(keygen2.key_params());
    rsa2.set_bits(1024);

    pgp_key_pkt_t seckey2;
    assert_false(keygen2.generate(seckey2, true));
}

TEST_F(rnp_tests, rnp_test_eddsa)
{
    rnp::KeygenParams keygen(PGP_PKA_EDDSA, global_ctx);
    pgp_key_pkt_t     seckey;
    assert_true(keygen.generate(seckey, true));

    rnp::secure_bytes hash(32);
    global_ctx.rng.get(hash.data(), hash.size());

    pgp::ECSigMaterial  sig(PGP_HASH_SHA256);
    pgp::RSASigMaterial sig2(PGP_HASH_SHA256);

    assert_rnp_failure(seckey.material->sign(global_ctx, sig2, hash));
    assert_rnp_failure(seckey.material->verify(global_ctx, sig2, hash));

    assert_rnp_success(seckey.material->sign(global_ctx, sig, hash));
    assert_rnp_success(seckey.material->verify(global_ctx, sig, hash));

    pgp::ECDHEncMaterial enc;
    assert_rnp_failure(seckey.material->encrypt(global_ctx, enc, hash));
    assert_rnp_failure(seckey.material->decrypt(global_ctx, hash, enc));

    // cut one byte off hash -> invalid sig
    rnp::secure_bytes hash_cut(31);
    assert_rnp_failure(seckey.material->verify(global_ctx, sig, hash_cut));

    // swap r/s -> invalid sig
    pgp::mpi tmp = sig.sig.r;
    sig.sig.r = sig.sig.s;
    sig.sig.s = tmp;
    assert_rnp_failure(seckey.material->verify(global_ctx, sig, hash));
}

TEST_F(rnp_tests, rnp_test_x25519)
{
    rnp::KeygenParams keygen(PGP_PKA_ECDH, global_ctx);
    auto &            ecc = dynamic_cast<pgp::ECCKeyParams &>(keygen.key_params());
    ecc.set_curve(PGP_CURVE_25519);

    pgp_key_pkt_t seckey;
    assert_true(keygen.generate(seckey, true));

    /* check for length and correctly tweaked bits */
    auto &ec = dynamic_cast<pgp::ECKeyMaterial &>(*seckey.material);
    assert_int_equal(ec.x().size(), 32);
    assert_int_equal(ec.x()[31] & 7, 0);
    assert_int_equal(ec.x()[0] & 128, 0);
    assert_int_equal(ec.x()[0] & 64, 64);
    /* encrypt */
    pgp::Fingerprint     fp(seckey);
    rnp::secure_bytes    in({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    pgp::ECDHEncMaterial enc;
    pgp::SM2EncMaterial  enc2;
    enc.enc.fp = fp.vec();
    assert_rnp_failure(seckey.material->encrypt(global_ctx, enc2, in));
    assert_rnp_success(seckey.material->encrypt(global_ctx, enc, in));
    assert_true(enc.enc.m.size() > 16);
    assert_int_equal(enc.enc.p[0], 0x40);
    assert_int_equal(enc.enc.p.size(), 33);
    /* decrypt */
    rnp::secure_bytes out;
    assert_rnp_failure(seckey.material->decrypt(global_ctx, out, enc2));
    assert_true(out.empty());
    assert_rnp_success(seckey.material->decrypt(global_ctx, out, enc));
    assert_int_equal(out.size(), 16);
    assert_int_equal(memcmp(in.data(), out.data(), 16), 0);
    /* negative cases */
    enc.enc.p[16] ^= 0xff;
    assert_rnp_failure(seckey.material->decrypt(global_ctx, out, enc));

    enc.enc.p[16] ^= 0xff;
    enc.enc.p[0] = 0x04;
    assert_rnp_failure(seckey.material->decrypt(global_ctx, out, enc));

    enc.enc.p[0] = 0x40;
    uint8_t back = enc.enc.m.back();
    enc.enc.m.pop_back();
    assert_rnp_failure(seckey.material->decrypt(global_ctx, out, enc));

    enc.enc.m.push_back(back);
    enc.enc.m.push_back(0);
    assert_rnp_failure(seckey.material->decrypt(global_ctx, out, enc));

    rnp::secure_bytes hash(32);
    global_ctx.rng.get(hash.data(), hash.size());
    pgp::ECSigMaterial sig(PGP_HASH_SHA256);
    assert_rnp_failure(seckey.material->sign(global_ctx, sig, hash));
    assert_rnp_failure(seckey.material->verify(global_ctx, sig, hash));
}

static void
elgamal_roundtrip(const pgp::eg::Key &key, rnp::RNG &rng)
{
    rnp::secure_bytes  in_b({0x01, 0x02, 0x03, 0x04, 0x17});
    pgp::eg::Encrypted enc = {{}};
    rnp::secure_bytes  res;

    assert_rnp_success(key.encrypt_pkcs1(rng, enc, in_b));
    assert_rnp_success(key.decrypt_pkcs1(rng, res, enc));
    assert_int_equal(res.size(), in_b.size());
    assert_true(bin_eq_hex(res.data(), res.size(), "0102030417"));
}

TEST_F(rnp_tests, raw_elgamal_random_key_test_success)
{
    pgp::eg::Key key;

    assert_rnp_success(key.generate(global_ctx.rng, 1024));
    assert_true(key.validate(true));
    elgamal_roundtrip(key, global_ctx.rng);
}

TEST_F(rnp_tests, ecdsa_signverify_success)
{
    const pgp_hash_alg_t hash_alg = PGP_HASH_SHA512;

    struct curve {
        pgp_curve_t id;
        size_t      size;
    } curves[] = {
      {PGP_CURVE_NIST_P_256, 32}, {PGP_CURVE_NIST_P_384, 48}, {PGP_CURVE_NIST_P_521, 64}};

    for (size_t i = 0; i < ARRAY_SIZE(curves); i++) {
        // Generate test data. Mainly to make valgrind not to complain about uninitialized data
        rnp::secure_bytes hash(rnp::Hash::size(hash_alg));
        global_ctx.rng.get(hash.data(), hash.size());

        rnp::KeygenParams keygen(PGP_PKA_ECDSA, global_ctx);
        keygen.set_hash(hash_alg);
        auto &ecc = dynamic_cast<pgp::ECCKeyParams &>(keygen.key_params());
        ecc.set_curve(curves[i].id);

        pgp_key_pkt_t seckey1;
        pgp_key_pkt_t seckey2;

        assert_true(keygen.generate(seckey1, true));
        assert_true(keygen.generate(seckey2, true));

        rnp::secure_bytes    in({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
        rnp::secure_bytes    out;
        pgp::ECDHEncMaterial enc;
        assert_rnp_failure(seckey1.material->encrypt(global_ctx, enc, in));
        assert_rnp_failure(seckey1.material->decrypt(global_ctx, out, enc));

        pgp::ECSigMaterial sig(hash_alg);
        assert_rnp_success(seckey1.material->sign(global_ctx, sig, hash));
        assert_rnp_success(seckey1.material->verify(global_ctx, sig, hash));

        // Fails because of different key used
        assert_rnp_failure(seckey2.material->verify(global_ctx, sig, hash));

        // Fails because message won't verify
        hash[0] = ~hash[0];
        assert_rnp_failure(seckey1.material->verify(global_ctx, sig, hash));
    }
}

TEST_F(rnp_tests, ecdh_roundtrip)
{
    struct curve {
        pgp_curve_t id;
        size_t      size;
    } curves[] = {
      {PGP_CURVE_NIST_P_256, 32}, {PGP_CURVE_NIST_P_384, 48}, {PGP_CURVE_NIST_P_521, 66}};

    rnp::secure_bytes in({1, 2, 3});
    in.insert(in.end(), 32 - in.size(), 0);

    for (size_t i = 0; i < ARRAY_SIZE(curves); i++) {
        rnp::KeygenParams keygen(PGP_PKA_ECDH, global_ctx);
        keygen.set_hash(PGP_HASH_SHA512);
        auto &ecc = dynamic_cast<pgp::ECCKeyParams &>(keygen.key_params());
        ecc.set_curve(curves[i].id);

        pgp_key_pkt_t ecdh_key1{};
        assert_true(keygen.generate(ecdh_key1, true));

        pgp::Fingerprint   ecdh_key1_fpr(ecdh_key1);
        pgp::ECSigMaterial sig(keygen.hash());
        rnp::secure_bytes  hash(rnp::Hash::size(keygen.hash()));
        assert_rnp_failure(ecdh_key1.material->sign(global_ctx, sig, hash));
        assert_rnp_failure(ecdh_key1.material->verify(global_ctx, sig, hash));

        pgp::ECDHEncMaterial enc;
        enc.enc.fp = ecdh_key1_fpr.vec();
        assert_rnp_success(ecdh_key1.material->encrypt(global_ctx, enc, in));

        rnp::secure_bytes res;
        assert_rnp_success(ecdh_key1.material->decrypt(global_ctx, res, enc));

        assert_int_equal(in.size(), res.size());
        assert_true(in == res);
    }
}

namespace pgp {
class ECDHTestKeyMaterial : public ECDHKeyMaterial {
  public:
    ECDHTestKeyMaterial(const ECDHKeyMaterial &src) : ECDHKeyMaterial(src)
    {
    }

    void
    set_key_wrap_alg(pgp_symm_alg_t alg)
    {
        key_.key_wrap_alg = alg;
    }

    ec::Key &
    ec()
    {
        return key_;
    }
};
} // namespace pgp

TEST_F(rnp_tests, ecdh_decryptionNegativeCases)
{
    rnp::secure_bytes in({1, 2, 3, 4});
    in.insert(in.end(), 32 - in.size(), 0);
    rnp::secure_bytes res;

    rnp::KeygenParams keygen(PGP_PKA_ECDH, global_ctx);
    keygen.set_hash(PGP_HASH_SHA512);
    auto &ecc = dynamic_cast<pgp::ECCKeyParams &>(keygen.key_params());
    ecc.set_curve(PGP_CURVE_NIST_P_256);

    pgp_key_pkt_t ecdh_key1;
    assert_true(keygen.generate(ecdh_key1, true));

    pgp::Fingerprint     ecdh_key1_fpr(ecdh_key1);
    pgp::ECDHEncMaterial enc;
    enc.enc.fp = ecdh_key1_fpr.vec();
    assert_rnp_success(ecdh_key1.material->encrypt(global_ctx, enc, in));

    auto m = enc.enc.m;
    enc.enc.m.resize(0);
    assert_int_equal(ecdh_key1.material->decrypt(global_ctx, res, enc), RNP_ERROR_GENERIC);

    enc.enc.m.assign(m.begin(), m.end() - 1);
    assert_int_equal(ecdh_key1.material->decrypt(global_ctx, res, enc), RNP_ERROR_GENERIC);

    pgp::ECDHTestKeyMaterial key1_mod(
      dynamic_cast<pgp::ECDHKeyMaterial &>(*ecdh_key1.material));
    key1_mod.set_key_wrap_alg(PGP_SA_IDEA);
    assert_int_equal(key1_mod.decrypt(global_ctx, res, enc), RNP_ERROR_NOT_SUPPORTED);
}

TEST_F(rnp_tests, sm2_roundtrip)
{
    rnp::KeygenParams keygen(PGP_PKA_SM2, global_ctx);
    keygen.set_hash(PGP_HASH_SM3);

    rnp::secure_bytes key(27, 0);
    global_ctx.rng.get(key.data(), key.size());

    pgp_key_pkt_t seckey;
#if defined(ENABLE_SM2)
    assert_true(keygen.generate(seckey, true));
    auto &eckey = *seckey.material;

    pgp_hash_alg_t       hashes[] = {PGP_HASH_SM3, PGP_HASH_SHA256, PGP_HASH_SHA512};
    pgp::SM2EncMaterial  enc;
    pgp::ECDHEncMaterial enc2;

    for (size_t i = 0; i < ARRAY_SIZE(hashes); ++i) {
        rnp::secure_bytes dec(32, 0);
        assert_rnp_failure(eckey.encrypt(global_ctx, enc2, key));
        assert_rnp_failure(eckey.decrypt(global_ctx, dec, enc2));
        assert_rnp_success(eckey.encrypt(global_ctx, enc, key));
        assert_rnp_success(eckey.decrypt(global_ctx, dec, enc));
        assert_true(dec == key);
    }
#else
    assert_false(keygen.generate(seckey, true));
#endif
}

#if defined(ENABLE_SM2)
TEST_F(rnp_tests, sm2_sm3_signature_test)
{
    const char *msg = "no backdoors here";

    pgp::ec::Key       sm2_key;
    pgp::ec::Signature sig;

    pgp_hash_alg_t hash_alg = PGP_HASH_SM3;
    const size_t   hash_len = rnp::Hash::size(hash_alg);

    sm2_key.curve = PGP_CURVE_NIST_P_256;

    hex2mpi(&sm2_key.p,
            "04d9a2025f1ab59bc44e35fc53aeb8e87a79787d30cd70a1f7c49e064b8b8a2fb24d8"
            "c82f49ee0a5b11df22cb0c3c6d9d5526d9e24d02ff8c83c06a859c26565f1");
    hex2mpi(&sm2_key.x, "110E7973206F68C19EE5F7328C036F26911C8C73B4E4F36AE3291097F8984FFC");

    assert_rnp_success(pgp::sm2::validate_key(global_ctx.rng, sm2_key, true));

    auto hash = rnp::Hash::create(hash_alg);

    assert_rnp_success(pgp::sm2::compute_za(sm2_key, *hash, "sm2_p256_test@example.com"));
    hash->add(msg, strlen(msg));
    rnp::secure_bytes digest = hash->sec_finish();
    assert_int_equal(digest.size(), hash_len);

    // First generate a signature, then verify it
    assert_rnp_success(pgp::sm2::sign(global_ctx.rng, sig, hash_alg, digest, sm2_key));
    assert_rnp_success(pgp::sm2::verify(sig, hash_alg, digest, sm2_key));

    // Check that invalid signatures are rejected
    digest[0] ^= 1;
    assert_rnp_failure(pgp::sm2::verify(sig, hash_alg, digest, sm2_key));

    digest[0] ^= 1;
    assert_rnp_success(pgp::sm2::verify(sig, hash_alg, digest, sm2_key));

    // Now verify a known good signature for this key/message (generated by GmSSL)
    hex2mpi(&sig.r, "96AA39A0C4A5C454653F394E86386F2E38BE14C57D0E555F3A27A5CEF30E51BD");
    hex2mpi(&sig.s, "62372BE4AC97DBE725AC0B279BB8FD15883858D814FD792DDB0A401DCC988E70");
    assert_rnp_success(pgp::sm2::verify(sig, hash_alg, digest, sm2_key));
}
#endif

#if defined(ENABLE_SM2)
TEST_F(rnp_tests, sm2_sha256_signature_test)
{
    const char *       msg = "hi chappy";
    pgp::ec::Key       sm2_key;
    pgp::ec::Signature sig;
    pgp_hash_alg_t     hash_alg = PGP_HASH_SHA256;
    const size_t       hash_len = rnp::Hash::size(hash_alg);

    sm2_key.curve = PGP_CURVE_SM2_P_256;
    hex2mpi(&sm2_key.p,
            "04d03d30dd01ca3422aeaccf9b88043b554659d3092b0a9e8cce3e8c4530a98cb79d7"
            "05e6213eee145b748e36e274e5f101dc10d7bbc9dab9a04022e73b76e02cd");
    hex2mpi(&sm2_key.x, "110E7973206F68C19EE5F7328C036F26911C8C73B4E4F36AE3291097F8984FFC");

    assert_rnp_success(pgp::sm2::validate_key(global_ctx.rng, sm2_key, true));

    auto hash = rnp::Hash::create(hash_alg);
    assert_rnp_success(pgp::sm2::compute_za(sm2_key, *hash, "sm2test@example.com"));
    hash->add(msg, strlen(msg));
    rnp::secure_bytes digest = hash->sec_finish();
    assert_int_equal(digest.size(), hash_len);

    // First generate a signature, then verify it
    assert_rnp_success(pgp::sm2::sign(global_ctx.rng, sig, hash_alg, digest, sm2_key));
    assert_rnp_success(pgp::sm2::verify(sig, hash_alg, digest, sm2_key));

    // Check that invalid signatures are rejected
    digest[0] ^= 1;
    assert_rnp_failure(pgp::sm2::verify(sig, hash_alg, digest, sm2_key));

    digest[0] ^= 1;
    assert_rnp_success(pgp::sm2::verify(sig, hash_alg, digest, sm2_key));

    // Now verify a known good signature for this key/message (generated by GmSSL)
    hex2mpi(&sig.r, "94DA20EA69E4FC70692158BF3D30F87682A4B2F84DF4A4829A1EFC5D9C979D3F");
    hex2mpi(&sig.s, "EE15AF8D455B728AB80E592FCB654BF5B05620B2F4D25749D263D5C01FAD365F");
    assert_rnp_success(pgp::sm2::verify(sig, hash_alg, digest, sm2_key));
}
#endif

TEST_F(rnp_tests, test_dsa_roundtrip)
{
    struct key_params {
        size_t         p;
        size_t         q;
        pgp_hash_alg_t h;
    } keys[] = {
      // all 1024 key-hash combinations
      {1024, 160, PGP_HASH_SHA1},
      {1024, 160, PGP_HASH_SHA224},
      {1024, 160, PGP_HASH_SHA256},
      {1024, 160, PGP_HASH_SHA384},
      {1024, 160, PGP_HASH_SHA512},
      // all 2048 key-hash combinations
      {2048, 256, PGP_HASH_SHA256},
      {2048, 256, PGP_HASH_SHA384},
      {2048, 256, PGP_HASH_SHA512},
      // misc
      {1088, 224, PGP_HASH_SHA512},
      {1024, 256, PGP_HASH_SHA256},
    };

    uint8_t message[PGP_MAX_HASH_SIZE];
    global_ctx.rng.get(message, sizeof(message));

    for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
        rnp::KeygenParams keygen(PGP_PKA_DSA, global_ctx);
        keygen.set_hash(keys[i].h);
        auto &dsa = dynamic_cast<pgp::DSAKeyParams &>(keygen.key_params());
        dsa.set_bits(keys[i].p);
        dsa.set_qbits(keys[i].q);

        pgp_key_pkt_t seckey;
        assert_true(keygen.generate(seckey, true));
        // try to prevent timeouts in travis-ci
        printf(
          "p: %zu q: %zu h: %s\n", dsa.bits(), dsa.qbits(), rnp::Hash::name(keygen.hash()));
        fflush(stdout);

        rnp::secure_bytes  in({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
        rnp::secure_bytes  out;
        pgp::EGEncMaterial enc;
        assert_rnp_failure(seckey.material->encrypt(global_ctx, enc, in));
        assert_rnp_failure(seckey.material->decrypt(global_ctx, out, enc));

        auto &              key = *seckey.material;
        rnp::secure_bytes   hash(message, message + rnp::Hash::size(keygen.hash()));
        pgp::DSASigMaterial sig(keygen.hash());
        assert_rnp_success(key.sign(global_ctx, sig, hash));
        assert_rnp_success(key.verify(global_ctx, sig, hash));
    }
}

TEST_F(rnp_tests, test_dsa_verify_negative)
{
    uint8_t       message[PGP_MAX_HASH_SIZE];
    pgp_key_pkt_t sec_key1;
    pgp_key_pkt_t sec_key2;

    global_ctx.rng.get(message, sizeof(message));

    rnp::KeygenParams keygen(PGP_PKA_DSA, global_ctx);
    keygen.set_hash(PGP_HASH_SHA1);
    auto &dsa = dynamic_cast<pgp::DSAKeyParams &>(keygen.key_params());
    dsa.set_bits(1024);
    dsa.set_qbits(160);

    assert_true(keygen.generate(sec_key1, true));
    // try to prevent timeouts in travis-ci
    printf("p: %zu q: %zu h: %s\n", dsa.bits(), dsa.qbits(), rnp::Hash::name(keygen.hash()));
    assert_true(keygen.generate(sec_key2, true));

    auto &key1 = *sec_key1.material;
    auto &key2 = *sec_key2.material;

    rnp::secure_bytes   hash(message, message + rnp::Hash::size(keygen.hash()));
    pgp::DSASigMaterial sig(keygen.hash());
    assert_rnp_success(key1.sign(global_ctx, sig, hash));
    // wrong key used
    assert_int_equal(key2.verify(global_ctx, sig, hash), RNP_ERROR_SIGNATURE_INVALID);
    // different message
    hash[0] = ~hash[0];
    assert_int_equal(key1.verify(global_ctx, sig, hash), RNP_ERROR_SIGNATURE_INVALID);
}

#if defined(ENABLE_PQC)
TEST_F(rnp_tests, kyber_ecdh_roundtrip)
{
    pgp_pubkey_alg_t algs[] = {PGP_PKA_KYBER768_X25519,
                               /* PGP_PKA_KYBER1024_X448,  */ // X448 not yet implemented
                               PGP_PKA_KYBER1024_P384,
                               PGP_PKA_KYBER768_BP256,
                               PGP_PKA_KYBER1024_BP384};

    rnp::secure_bytes in(32, 0);
    rnp::secure_bytes res(36);

    for (size_t i = 0; i < in.size(); i++) {
        in[i] = i; // assures that we do not have a special case with all-zeroes
    }

    for (size_t i = 0; i < ARRAY_SIZE(algs); i++) {
        rnp::KeygenParams keygen(algs[i], global_ctx);
        keygen.set_hash(PGP_HASH_SHA512);

        pgp_key_pkt_t key_pkt;
        assert_true(keygen.generate(key_pkt, true));

        pgp::MlkemEcdhEncMaterial enc(algs[i]);
        assert_rnp_success(key_pkt.material->encrypt(global_ctx, enc, in));
        assert_rnp_success(key_pkt.material->decrypt(global_ctx, res, enc));

        assert_int_equal(in.size(), res.size());
        assert_true(in == res);
    }
}

TEST_F(rnp_tests, dilithium_exdsa_signverify_success)
{
    uint8_t              message[64];
    const pgp_hash_alg_t hash_alg = PGP_HASH_SHA512;

    pgp_pubkey_alg_t algs[] = {PGP_PKA_DILITHIUM3_ED25519,
                               /* PGP_PKA_DILITHIUM5_ED448,*/ PGP_PKA_DILITHIUM3_P256,
                               PGP_PKA_DILITHIUM5_P384,
                               PGP_PKA_DILITHIUM3_BP256,
                               PGP_PKA_DILITHIUM5_BP384};

    for (size_t i = 0; i < ARRAY_SIZE(algs); i++) {
        // Generate test data. Mainly to make valgrind not to complain about uninitialized data
        global_ctx.rng.get(message, sizeof(message));

        rnp::KeygenParams keygen(algs[i], global_ctx);
        keygen.set_hash(hash_alg);

        pgp_key_pkt_t seckey1;
        pgp_key_pkt_t seckey2;

        assert_true(keygen.generate(seckey1, true));
        assert_true(keygen.generate(seckey2, true));

        auto &key1 = *seckey1.material;
        auto &key2 = *seckey2.material;

        pgp::DilithiumSigMaterial sig(keygen.alg(), keygen.hash());
        sig.halg = hash_alg;
        rnp::secure_bytes hash(message, message + sizeof(message));
        assert_rnp_success(key1.sign(global_ctx, sig, hash));
        assert_rnp_success(key1.verify(global_ctx, sig, hash));

        // Fails because of different key used
        assert_rnp_failure(key2.verify(global_ctx, sig, hash));
    }
}

TEST_F(rnp_tests, sphincsplus_signverify_success)
{
    uint8_t                 message[64];
    pgp_pubkey_alg_t        algs[] = {PGP_PKA_SPHINCSPLUS_SHA2, PGP_PKA_SPHINCSPLUS_SHAKE};
    sphincsplus_parameter_t params[] = {sphincsplus_simple_128s,
                                        sphincsplus_simple_128f,
                                        sphincsplus_simple_192s,
                                        sphincsplus_simple_192f,
                                        sphincsplus_simple_256s,
                                        sphincsplus_simple_256f};

    for (size_t i = 0; i < ARRAY_SIZE(algs); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(params); j++) {
            // Generate test data. Mainly to make valgrind not to complain about uninitialized
            // data
            global_ctx.rng.get(message, sizeof(message));

            rnp::KeygenParams keygen(algs[i], global_ctx);
            auto &slhdsa = dynamic_cast<pgp::SlhdsaKeyParams &>(keygen.key_params());
            slhdsa.set_param(params[j]);

            pgp_key_pkt_t seckey1;
            pgp_key_pkt_t seckey2;

            assert_true(keygen.generate(seckey1, true));
            assert_true(keygen.generate(seckey2, true));

            auto &                 key1 = *seckey1.material;
            auto &                 key2 = *seckey2.material;
            rnp::secure_bytes      hash(message, message + sizeof(message));
            pgp::SlhdsaSigMaterial sig(keygen.hash());
            assert_rnp_success(key1.sign(global_ctx, sig, hash));
            assert_rnp_success(key1.verify(global_ctx, sig, hash));

            // Fails because of different key used
            assert_rnp_failure(key2.verify(global_ctx, sig, hash));
        }
    }
}
#endif

// platforms known to not have a robust response can compile with
// -DS2K_MINIMUM_TUNING_RATIO=2 (or whatever they need)
#ifndef S2K_MINIMUM_TUNING_RATIO
#define S2K_MINIMUM_TUNING_RATIO 4
#endif

TEST_F(rnp_tests, s2k_iteration_tuning)
{
    pgp_hash_alg_t hash_alg = PGP_HASH_SHA512;

    /*
    Run trials for a while (1/4 second) to ensure dynamically clocked
    cores spin up to full speed.
    */
    const size_t TRIAL_MSEC = 250;

    const size_t iters_100 = pgp_s2k_compute_iters(hash_alg, 100, TRIAL_MSEC);
    const size_t iters_10 = pgp_s2k_compute_iters(hash_alg, 10, TRIAL_MSEC);

    double ratio = static_cast<double>(iters_100) / iters_10;
    printf("s2k iteration tuning ratio: %g, (%zu:%zu)\n", ratio, iters_10, iters_100);
    // Test roughly linear cost, often skeyed by clock idle
    assert_greater_than(ratio, S2K_MINIMUM_TUNING_RATIO);

    // Should not crash for unknown hash algorithm
    assert_int_equal(pgp_s2k_compute_iters(PGP_HASH_UNKNOWN, 1000, TRIAL_MSEC), 0);
    /// TODO test that hashing iters_xx data takes roughly requested time

    size_t iter_sha1 = global_ctx.s2k_iterations(PGP_HASH_SHA1);
    assert_int_equal(iter_sha1, global_ctx.s2k_iterations(PGP_HASH_SHA1));
    size_t iter_sha512 = global_ctx.s2k_iterations(PGP_HASH_SHA512);
    assert_int_equal(iter_sha512, global_ctx.s2k_iterations(PGP_HASH_SHA512));
    assert_int_equal(global_ctx.s2k_iterations(PGP_HASH_UNKNOWN), 0);
}

TEST_F(rnp_tests, s2k_iteration_encode_decode)
{
    const size_t MAX_ITER = 0x3e00000; // 0x1F << (0xF + 6);
    // encoding tests
    assert_int_equal(pgp_s2k_encode_iterations(0), 0);
    assert_int_equal(pgp_s2k_encode_iterations(512), 0);
    assert_int_equal(pgp_s2k_encode_iterations(1024), 0);
    assert_int_equal(pgp_s2k_encode_iterations(1024), 0);
    assert_int_equal(pgp_s2k_encode_iterations(1025), 1);
    assert_int_equal(pgp_s2k_encode_iterations(1088), 1);
    assert_int_equal(pgp_s2k_encode_iterations(1089), 2);
    assert_int_equal(pgp_s2k_encode_iterations(2048), 16);
    assert_int_equal(pgp_s2k_encode_iterations(MAX_ITER - 1), 0xFF);
    assert_int_equal(pgp_s2k_encode_iterations(MAX_ITER), 0xFF);
    assert_int_equal(pgp_s2k_encode_iterations(MAX_ITER + 1), 0xFF);
    assert_int_equal(pgp_s2k_encode_iterations(SIZE_MAX), 0xFF);
    // decoding tests
    assert_int_equal(pgp_s2k_decode_iterations(0), 1024);
    assert_int_equal(pgp_s2k_decode_iterations(1), 1088);
    assert_int_equal(pgp_s2k_decode_iterations(16), 2048);
    assert_int_equal(pgp_s2k_decode_iterations(0xFF), MAX_ITER);
}

static bool
read_key_pkt(pgp_key_pkt_t *key, const char *path)
{
    pgp_source_t src = {};
    if (init_file_src(&src, path)) {
        return false;
    }
    bool res = !key->parse(src);
    src.close();
    return res;
}

namespace pgp {
class RSATestKeyMaterial : public RSAKeyMaterial {
  public:
    RSATestKeyMaterial(const RSAKeyMaterial &src) : RSAKeyMaterial(src)
    {
    }

    rsa::Key &
    rsa()
    {
        return key_;
    }
};

class DSATestKeyMaterial : public DSAKeyMaterial {
  public:
    DSATestKeyMaterial(const DSAKeyMaterial &src) : DSAKeyMaterial(src)
    {
    }

    dsa::Key &
    dsa()
    {
        return key_;
    }
};

class EGTestKeyMaterial : public EGKeyMaterial {
  public:
    EGTestKeyMaterial(const EGKeyMaterial &src) : EGKeyMaterial(src)
    {
    }

    eg::Key &
    eg()
    {
        return key_;
    }
};

class ECDSATestKeyMaterial : public ECDSAKeyMaterial {
  public:
    ECDSATestKeyMaterial(const ECDSAKeyMaterial &src) : ECDSAKeyMaterial(src)
    {
    }

    ec::Key &
    ec()
    {
        return key_;
    }
};

class EDDSATestKeyMaterial : public EDDSAKeyMaterial {
  public:
    EDDSATestKeyMaterial(const EDDSAKeyMaterial &src) : EDDSAKeyMaterial(src)
    {
    }

    ec::Key &
    ec()
    {
        return key_;
    }
};

} // namespace pgp

#define KEYS "data/test_validate_key_material/"

TEST_F(rnp_tests, test_validate_key_material)
{
    pgp_key_pkt_t key;

    /* RSA key and subkey */
    assert_true(read_key_pkt(&key, KEYS "rsa-pub.pgp"));
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    pgp::RSATestKeyMaterial rkey(dynamic_cast<pgp::RSAKeyMaterial &>(*key.material));
    rkey.rsa().n[rkey.rsa().n.size() - 1] &= ~1;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    rkey.rsa().n[rkey.rsa().n.size() - 1] |= 1;
    rkey.rsa().e[rkey.rsa().e.size() - 1] &= ~1;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    key = pgp_key_pkt_t();

    assert_true(read_key_pkt(&key, KEYS "rsa-sub.pgp"));
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    rkey = pgp::RSATestKeyMaterial(dynamic_cast<pgp::RSAKeyMaterial &>(*key.material));
    rkey.rsa().n[rkey.rsa().n.size() - 1] &= ~1;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    rkey.rsa().n[rkey.rsa().n.size() - 1] |= 1;
    rkey.rsa().e[rkey.rsa().e.size() - 1] &= ~1;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    key = pgp_key_pkt_t();

    assert_true(read_key_pkt(&key, KEYS "rsa-sec.pgp"));
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    assert_true(key.material->validity().valid);
    assert_true(key.material->validity().validated);
    assert_rnp_success(decrypt_secret_key(&key, NULL));
    /* make sure validity is reset after decryption */
    assert_false(key.material->validity().valid);
    assert_false(key.material->validity().validated);
    assert_true(key.material->secret());
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    rkey = pgp::RSATestKeyMaterial(dynamic_cast<pgp::RSAKeyMaterial &>(*key.material));
    rkey.rsa().e[rkey.rsa().e.size() - 1] += 1;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    rkey.rsa().e[rkey.rsa().e.size() - 1] -= 1;
    rkey.rsa().p[rkey.rsa().p.size() - 1] += 2;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    rkey.rsa().p[rkey.rsa().p.size() - 1] -= 2;
    rkey.rsa().q[rkey.rsa().q.size() - 1] += 2;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    rkey.rsa().q[rkey.rsa().q.size() - 1] -= 2;
    rkey.validate(global_ctx);
    assert_true(rkey.valid());
    key = pgp_key_pkt_t();

    assert_true(read_key_pkt(&key, KEYS "rsa-ssb.pgp"));
    assert_rnp_success(decrypt_secret_key(&key, NULL));
    assert_true(key.material->secret());
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    rkey = pgp::RSATestKeyMaterial(dynamic_cast<pgp::RSAKeyMaterial &>(*key.material));
    rkey.rsa().e[rkey.rsa().e.size() - 1] += 1;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    rkey.rsa().e[rkey.rsa().e.size() - 1] -= 1;
    rkey.rsa().p[rkey.rsa().p.size() - 1] += 2;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    rkey.rsa().p[rkey.rsa().p.size() - 1] -= 2;
    rkey.rsa().q[rkey.rsa().q.size() - 1] += 2;
    rkey.validate(global_ctx);
    assert_false(rkey.valid());
    rkey.rsa().q[rkey.rsa().q.size() - 1] -= 2;
    rkey.validate(global_ctx);
    assert_true(rkey.valid());
    key = pgp_key_pkt_t();

    /* DSA-ElGamal key */
    assert_true(read_key_pkt(&key, KEYS "dsa-sec.pgp"));
    pgp::DSATestKeyMaterial dkey(dynamic_cast<pgp::DSAKeyMaterial &>(*key.material));
    dkey.dsa().q[dkey.dsa().q.size() - 1] += 2;
    dkey.validate(global_ctx);
    assert_false(dkey.valid());
    dkey.dsa().q[dkey.dsa().q.size() - 1] -= 2;
    assert_rnp_success(decrypt_secret_key(&key, NULL));
    assert_true(key.material->secret());
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    dkey = pgp::DSATestKeyMaterial(dynamic_cast<pgp::DSAKeyMaterial &>(*key.material));
    dkey.dsa().y[dkey.dsa().y.size() - 1] += 2;
    dkey.validate(global_ctx);
    assert_false(dkey.valid());
    dkey.dsa().y[dkey.dsa().y.size() - 1] -= 2;
    dkey.dsa().p[dkey.dsa().p.size() - 1] += 2;
    dkey.validate(global_ctx);
    assert_false(dkey.valid());
    dkey.dsa().p[dkey.dsa().p.size() - 1] -= 2;
    /* since Botan calculates y from x on key load we do not check x vs y */
    dkey.dsa().x = dkey.dsa().q;
    dkey.validate(global_ctx);
    assert_false(dkey.valid());
    key = pgp_key_pkt_t();

    assert_true(read_key_pkt(&key, KEYS "eg-sec.pgp"));
    pgp::EGTestKeyMaterial gkey(dynamic_cast<pgp::EGKeyMaterial &>(*key.material));
    gkey.eg().p[gkey.eg().p.size() - 1] += 2;
    gkey.validate(global_ctx);
    assert_false(gkey.valid());
    gkey.eg().p[gkey.eg().p.size() - 1] -= 2;
    assert_rnp_success(decrypt_secret_key(&key, NULL));
    assert_true(key.material->secret());
    gkey = pgp::EGTestKeyMaterial(dynamic_cast<pgp::EGKeyMaterial &>(*key.material));
    gkey.validate(global_ctx);
    assert_true(gkey.valid());
    gkey.eg().p[gkey.eg().p.size() - 1] += 2;
    gkey.validate(global_ctx);
    assert_false(gkey.valid());
    gkey.eg().p[gkey.eg().p.size() - 1] -= 2;
    /* since Botan calculates y from x on key load we do not check x vs y */
    gkey.eg().x = gkey.eg().p;
    gkey.validate(global_ctx);
    assert_false(gkey.valid());
    key = pgp_key_pkt_t();

    /* ElGamal key with small subgroup */
    assert_true(read_key_pkt(&key, KEYS "eg-sec-small-group.pgp"));
    key.material->validate(global_ctx);
    assert_false(key.material->valid());
    assert_rnp_success(decrypt_secret_key(&key, NULL));
    key = pgp_key_pkt_t();

    assert_true(read_key_pkt(&key, KEYS "eg-sec-small-group-enc.pgp"));
    key.material->validate(global_ctx);
    assert_false(key.material->valid());
    assert_rnp_success(decrypt_secret_key(&key, "password"));
    key = pgp_key_pkt_t();

    /* ECDSA key */
    assert_true(read_key_pkt(&key, KEYS "ecdsa-p256-sec.pgp"));
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    pgp::ECDSATestKeyMaterial ekey(dynamic_cast<pgp::ECDSAKeyMaterial &>(*key.material));
    ekey.validate(global_ctx);
    assert_true(ekey.valid());
    ekey.ec().p[0] += 2;
    ekey.validate(global_ctx);
    assert_false(ekey.valid());
    ekey.ec().p[0] -= 2;
    ekey.ec().p[10] += 2;
    ekey.validate(global_ctx);
    assert_false(ekey.valid());
    ekey.ec().p[10] -= 2;
    assert_rnp_success(decrypt_secret_key(&key, NULL));
    assert_true(key.material->secret());
    key = pgp_key_pkt_t();

    /* ECDH key */
    assert_true(read_key_pkt(&key, KEYS "ecdh-p256-sec.pgp"));
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    pgp::ECDHTestKeyMaterial ehkey(dynamic_cast<pgp::ECDHKeyMaterial &>(*key.material));
    ehkey.ec().p[0] += 2;
    ehkey.validate(global_ctx);
    assert_false(ehkey.valid());
    ehkey.ec().p[0] -= 2;
    ehkey.ec().p[10] += 2;
    ehkey.validate(global_ctx);
    assert_false(ehkey.valid());
    ehkey.ec().p[10] -= 2;
    assert_rnp_success(decrypt_secret_key(&key, NULL));
    assert_true(key.material->secret());
    key = pgp_key_pkt_t();

    /* EDDSA key, just test for header since any value can be secret key */
    assert_true(read_key_pkt(&key, KEYS "ed25519-sec.pgp"));
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    pgp::EDDSATestKeyMaterial edkey(dynamic_cast<pgp::EDDSAKeyMaterial &>(*key.material));
    edkey.ec().p[0] += 2;
    edkey.validate(global_ctx);
    assert_false(edkey.valid());
    edkey.ec().p[0] -= 2;
    key = pgp_key_pkt_t();

    /* x25519 key, same as the previous - botan calculates pub key from the secret one */
    assert_true(read_key_pkt(&key, KEYS "x25519-sec.pgp"));
    key.material->validate(global_ctx);
    assert_true(key.material->valid());
    ehkey = pgp::ECDHTestKeyMaterial(dynamic_cast<pgp::ECDHKeyMaterial &>(*key.material));
    ehkey.ec().p[0] += 2;
    ehkey.validate(global_ctx);
    assert_false(ehkey.valid());
    ehkey.ec().p[0] -= 2;
    key = pgp_key_pkt_t();
}

TEST_F(rnp_tests, test_sm2_enabled)
{
    char *features = NULL;
    bool  supported = false;
    /* check whether FFI returns value which corresponds to defines */
#if defined(ENABLE_SM2)
    assert_true(sm2_enabled());
    /* SM2 */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_PK_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("SM2") != std::string::npos);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "SM2", &supported));
    assert_true(supported);
    /* SM3 */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_HASH_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("SM3") != std::string::npos);
    rnp_buffer_destroy(features);
    supported = false;
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SM3", &supported));
    assert_true(supported);
    /* SM4 */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_SYMM_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("SM4") != std::string::npos);
    rnp_buffer_destroy(features);
    supported = false;
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "SM4", &supported));
    assert_true(supported);
    /* Curve */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_CURVE, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("SM2 P-256") != std::string::npos);
    rnp_buffer_destroy(features);
    supported = false;
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "SM2 P-256", &supported));
    assert_true(supported);
#else
    assert_false(sm2_enabled());
    /* SM2 */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_PK_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("SM2") == std::string::npos);
    rnp_buffer_destroy(features);
    supported = true;
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "SM2", &supported));
    assert_false(supported);
    /* SM3 */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_HASH_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("SM3") == std::string::npos);
    rnp_buffer_destroy(features);
    supported = true;
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SM3", &supported));
    assert_false(supported);
    /* SM4 */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_SYMM_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("SM4") == std::string::npos);
    rnp_buffer_destroy(features);
    supported = true;
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "SM4", &supported));
    assert_false(supported);
    /* Curve */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_CURVE, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("SM2 P-256") == std::string::npos);
    rnp_buffer_destroy(features);
    supported = true;
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "SM2 P-256", &supported));
    assert_false(supported);
#endif
}

TEST_F(rnp_tests, test_aead_enabled)
{
    char *features = NULL;
    bool  supported = false;
    /* check whether FFI returns value which corresponds to defines */
#if defined(ENABLE_AEAD)
    bool has_eax = aead_eax_enabled();
    bool has_ocb = aead_ocb_enabled();
    assert_true(has_eax || has_ocb);
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_AEAD_ALG, &features));
    assert_non_null(features);
    assert_true((std::string(features).find("EAX") != std::string::npos) == has_eax);
    assert_true((std::string(features).find("OCB") != std::string::npos) == has_ocb);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "EAX", &supported));
    assert_true(supported == has_eax);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "OCB", &supported));
    assert_true(supported == has_ocb);
#else
    assert_false(aead_eax_enabled());
    assert_false(aead_ocb_enabled());
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_AEAD_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("EAX") == std::string::npos);
    assert_true(std::string(features).find("OCB") == std::string::npos);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "EAX", &supported));
    assert_false(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "OCB", &supported));
    assert_false(supported);
#endif
}

TEST_F(rnp_tests, test_idea_enabled)
{
    char *features = NULL;
    bool  supported = false;
    /* check whether FFI returns value which corresponds to defines */
#if defined(ENABLE_IDEA)
    assert_true(idea_enabled());
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_SYMM_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("IDEA") != std::string::npos);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "IDEA", &supported));
    assert_true(supported);
#else
    assert_false(idea_enabled());
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_SYMM_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("IDEA") == std::string::npos);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "IDEA", &supported));
    assert_false(supported);
#endif
}

TEST_F(rnp_tests, test_twofish_enabled)
{
    char *features = NULL;
    bool  supported = false;
    /* check whether FFI returns value which corresponds to defines */
#if defined(ENABLE_TWOFISH)
    assert_true(twofish_enabled());
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_SYMM_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("TWOFISH") != std::string::npos);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "TWOFISH", &supported));
    assert_true(supported);
#else
    assert_false(twofish_enabled());
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_SYMM_ALG, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("TWOFISH") == std::string::npos);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "TWOFISH", &supported));
    assert_false(supported);
#endif
}

TEST_F(rnp_tests, test_brainpool_enabled)
{
    char *features = NULL;
    bool  supported = false;
    /* check whether FFI returns value which corresponds to defines */
#if defined(ENABLE_BRAINPOOL)
    assert_true(brainpool_enabled());
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_CURVE, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("brainpool") != std::string::npos);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP256r1", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP384r1", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP512r1", &supported));
    assert_true(supported);
#else
    assert_false(brainpool_enabled());
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_CURVE, &features));
    assert_non_null(features);
    assert_true(std::string(features).find("brainpool") == std::string::npos);
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP256r1", &supported));
    assert_false(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP384r1", &supported));
    assert_false(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP512r1", &supported));
    assert_false(supported);
#endif
}

#if defined(CRYPTO_BACKEND_BOTAN)
TEST_F(rnp_tests, test_windows_botan_crash)
{
    /* Reproducer for https://github.com/randombit/botan/issues/3812 . Related CLI test
     * test_sym_encrypted__rnp_aead_botan_crash */

    auto data = file_to_vec("data/test_messages/message.aead-windows-issue-botan");
    /* First 32 bytes are encrypted key as it was extracted from the OpenPGP stream, so
     * skipping. */
    uint8_t *idx = data.data() + 32;
    uint8_t  bufbin[64] = {0};
    uint8_t  outbuf[32768] = {0};
    size_t   outsz = sizeof(outbuf);
    size_t   written = 0;
    size_t   read = 0;
    size_t   diff = 0;

    /* Now the data which exposes a possible crash */
    struct botan_cipher_struct *cipher = NULL;
    assert_int_equal(botan_cipher_init(&cipher, "AES-128/OCB", BOTAN_CIPHER_INIT_FLAG_DECRYPT),
                     0);

    const char *key2 = "417835a476bc5958b18d41fb00cf682d";
    assert_int_equal(rnp::hex_decode(key2, bufbin, 16), 16);
    assert_int_equal(botan_cipher_set_key(cipher, bufbin, 16), 0);

    const char *ad2 = "d40107020c0000000000000000";
    assert_int_equal(rnp::hex_decode(ad2, bufbin, 13), 13);
    assert_int_equal(botan_cipher_set_associated_data(cipher, bufbin, 13), 0);

    const char *nonce2 = "005dbbbe0088f9d17ca2d8d464920f";
    assert_int_equal(rnp::hex_decode(nonce2, bufbin, 15), 15);
    assert_int_equal(botan_cipher_start(cipher, bufbin, 15), 0);

    assert_int_equal(
      botan_cipher_update(cipher, 0, outbuf, outsz, &written, idx, 32736, &read), 0);
    diff = 32736 - read;
    idx += read;

    assert_int_equal(
      botan_cipher_update(cipher, 0, outbuf, outsz, &written, idx, diff + 32736, &read), 0);
    idx += read;
    diff = diff + 32736 - read;

    assert_int_equal(
      botan_cipher_update(cipher, 0, outbuf, outsz, &written, idx, diff + 32736, &read), 0);
    idx += read;
    diff = diff + 32736 - read;

    assert_int_equal(
      botan_cipher_update(cipher, 0, outbuf, outsz, &written, idx, diff + 32736, &read), 0);
    idx += read;
    diff = diff + 32736 - read;

    uint32_t ver_major = botan_version_major();
    uint32_t ver_minor = botan_version_minor();
    uint32_t ver_patch = botan_version_patch();
    uint32_t ver = (ver_major << 16) | (ver_minor << 8) | ver_patch;
    uint32_t ver_2_19_3 = (2 << 16) | (19 << 8) | 3;
    uint32_t ver_3_2_0 = (3 << 16) | (2 << 8);
    bool     check = true;
    /* Currently AV happens with versions up to 2.19.3 and 3.2.0 */
    if ((ver_major == 2) && (ver <= ver_2_19_3)) {
        check = false;
    }
    if ((ver_major == 3) && (ver <= ver_3_2_0)) {
        check = false;
    }

    if (check) {
        assert_int_equal(botan_cipher_update(cipher,
                                             BOTAN_CIPHER_UPDATE_FLAG_FINAL,
                                             outbuf,
                                             outsz,
                                             &written,
                                             idx,
                                             diff + 25119,
                                             &read),
                         0);
    }

    assert_int_equal(botan_cipher_reset(cipher), 0);
    assert_int_equal(botan_cipher_destroy(cipher), 0);
}
#endif
