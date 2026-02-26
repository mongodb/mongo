/*
 * Copyright (c) 2017-2019 [Ribose Inc](https://www.ribose.com).
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

#include "key.hpp"

#include "rnp_tests.h"
#include "support.h"

TEST_F(rnp_tests, test_invalid_g10)
{
    rnp::KeyProvider key_provider(rnp_key_provider_store);
    // load pubring
    auto pub_store =
      new rnp::KeyStore("data/keyrings/3/pubring.kbx", global_ctx, rnp::KeyFormat::KBX);
    assert_true(pub_store->load());
    // trigger "Unsupported public key algorithm:" error message
    auto sec_store = new rnp::KeyStore(
      "data/test_invalid_g10/private-keys-v1.d", global_ctx, rnp::KeyFormat::G10);
    key_provider.userdata = pub_store;
    assert_true(sec_store->load(&key_provider));
    // NULL key_provider
    assert_true(sec_store->load());

    delete pub_store;
    delete sec_store;
}

/* This test loads G10 keyrings and verifies certain properties
 * of the keys are correct.
 */
TEST_F(rnp_tests, test_load_g10)
{
    rnp::KeyProvider key_provider(rnp_key_provider_store);

    // load pubring
    auto pub_store =
      new rnp::KeyStore("data/keyrings/3/pubring.kbx", global_ctx, rnp::KeyFormat::KBX);
    assert_true(pub_store->load());
    // load secring
    auto sec_store =
      new rnp::KeyStore("data/keyrings/3/private-keys-v1.d", global_ctx, rnp::KeyFormat::G10);
    key_provider.userdata = pub_store;
    assert_true(sec_store->load(&key_provider));

    /* check primary key and subkey */
    test_load_gpg_check_key(pub_store, sec_store, "4BE147BB22DF1E60");
    test_load_gpg_check_key(pub_store, sec_store, "A49BAE05C16E8BC8");

    // cleanup
    delete pub_store;
    delete sec_store;

    /* another store */
    pub_store = new rnp::KeyStore(
      "data/test_stream_key_load/g10/pubring.kbx", global_ctx, rnp::KeyFormat::KBX);
    assert_true(pub_store->load());
    sec_store = new rnp::KeyStore(
      "data/test_stream_key_load/g10/private-keys-v1.d", global_ctx, rnp::KeyFormat::G10);
    key_provider.userdata = pub_store;
    assert_true(sec_store->load(&key_provider));

    /* dsa/eg key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "C8A10A7D78273E10"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "02A5715C3537717E"));

    /* rsa/rsa key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "2FB9179118898E8B"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "6E2F73008F8B8D6E"));

#ifdef CRYPTO_BACKEND_BOTAN
    /*  GnuPG extended key format requires AEAD support that is available for BOTAN backend
       only https://github.com/rnpgp/rnp/issues/1642 (???)
    */
    /* rsa/rsa new key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "BD860A52D1899C0F"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "8E08D46A37414996"));
#endif

    /* ed25519 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "CC786278981B0728"));

    /* ed25519/x25519 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "941822A0FC1B30A5"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "C711187E594376AF"));

    /* p-256/p-256 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "23674F21B2441527"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "37E285E9E9851491"));

    /* p-384/p-384 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "242A3AA5EA85F44A"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "E210E3D554A4FAD9"));

    /* p-521/p-521 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "2092CA8324263B6A"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "9853DF2F6D297442"));

    /* p256k1/p156k1 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "3EA5BB6F9692C1A0"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "7635401F90D3E533"));

    delete pub_store;
    delete sec_store;
}
