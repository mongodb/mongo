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

/* This test loads G23 keyring and verifies that certain properties
 * of the keys are correct.
 */
TEST_F(rnp_tests, test_load_g23)
{
    rnp::KeyProvider key_provider(rnp_key_provider_store);

    /* another store */
    auto pub_store = new rnp::KeyStore(
      "data/test_stream_key_load/g23/pubring.kbx", global_ctx, rnp::KeyFormat::KBX);
    assert_true(pub_store->load());
    auto sec_store = new rnp::KeyStore(
      "data/test_stream_key_load/g23/private-keys-v1.d", global_ctx, rnp::KeyFormat::G10);
    key_provider.userdata = pub_store;
    assert_true(sec_store->load(&key_provider));

#ifdef CRYPTO_BACKEND_BOTAN
    /*  GnuPG extended key format requires AEAD support that is available for BOTAN backend
       only https://github.com/rnpgp/rnp/issues/1642 (???)
    */
    /* dsa/elg key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "2651229E2D4DADF5"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "740AB758FAF0D5B7"));

    /* rsa/rsa key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "D1EF5C27C1F76F88"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "1F4E4EBC86A6E667"));

    /* ed25519 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "4F92A7A7B285CA0F"));

    /* ed25519/cv25519 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "0C96377D972E906C"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "8270B09D57420327"));

    /* p-256/p-256 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "AA1DEBEA6C10FCC6"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "8F511690EADC47F8"));

    /* p-384/p-384 key */
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "62774CDB7B085FB6"));
    assert_true(test_load_gpg_check_key(pub_store, sec_store, "F0D076E2A3876399"));
#else
    assert_false(test_load_gpg_check_key(pub_store, sec_store, "2651229E2D4DADF5"));
#endif // CRYPTO_BACKEND_BOTAN

    /* Wrong id -- no public key*/
    assert_false(test_load_gpg_check_key(pub_store, sec_store, "2651229E2D4DADF6"));

    /* Correct id but no private key*/
    assert_false(test_load_gpg_check_key(pub_store, sec_store, "25810145A8D4699A"));

    delete pub_store;
    delete sec_store;
}
