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

#include "rnp.h"
#include <rekey/rnp_key_store.h>
#include "rnp_tests.h"
#include "support.h"
#include "key.hpp"
#include "keygen.hpp"

static const rnp::Signature *
find_subsig(const rnp::Key *key, const char *userid)
{
    // find the userid index
    int uididx = -1;
    for (unsigned i = 0; i < key->uid_count(); i++) {
        if (key->get_uid(i).str == userid) {
            uididx = i;
            break;
        }
    }
    if (uididx == -1) {
        return NULL;
    }
    // find the subsig index
    for (size_t i = 0; i < key->sig_count(); i++) {
        auto &subsig = key->get_sig(i);
        if ((int) subsig.uid == uididx) {
            return &subsig;
        }
    }
    return NULL;
}

TEST_F(rnp_tests, test_load_user_prefs)
{
    auto pubring = new rnp::KeyStore("data/keyrings/1/pubring.gpg", global_ctx);
    assert_true(pubring->load());
    assert_int_equal(pubring->key_count(), 7);

    {
        const char *userid = "key1-uid0";

        // find the key
        rnp::Key *key = nullptr;
        assert_non_null(key = rnp_tests_key_search(pubring, userid));

        auto subsig = find_subsig(key, userid);
        assert_non_null(subsig);

        const rnp::UserPrefs prefs(subsig->sig);

        // symm algs
        std::vector<uint8_t> expected = {PGP_SA_AES_192, PGP_SA_CAST5};
        assert_true(prefs.symm_algs == expected);
        // hash algs
        expected = {PGP_HASH_SHA1, PGP_HASH_SHA224};
        assert_true(prefs.hash_algs == expected);
        // compression algs
        expected = {PGP_C_ZIP, PGP_C_NONE};
        assert_true(prefs.z_algs == expected);
        // key server prefs
        expected = {PGP_KEY_SERVER_NO_MODIFY};
        assert_true(prefs.ks_prefs == expected);
        // preferred key server
        assert_string_equal(prefs.key_server.c_str(), "hkp://pgp.mit.edu");
    }

    {
        const char *userid = "key0-uid0";

        // find the key
        rnp::Key *key = nullptr;
        assert_non_null(key = rnp_tests_key_search(pubring, userid));

        auto subsig = find_subsig(key, userid);
        assert_non_null(subsig);

        const rnp::UserPrefs prefs(subsig->sig);
        // symm algs
        std::vector<uint8_t> expected = {PGP_SA_AES_256,
                                         PGP_SA_AES_192,
                                         PGP_SA_AES_128,
                                         PGP_SA_CAST5,
                                         PGP_SA_TRIPLEDES,
                                         PGP_SA_IDEA};
        assert_true(prefs.symm_algs == expected);
        // hash algs
        expected = {
          PGP_HASH_SHA256, PGP_HASH_SHA1, PGP_HASH_SHA384, PGP_HASH_SHA512, PGP_HASH_SHA224};
        assert_true(prefs.hash_algs == expected);
        // compression algs
        expected = {PGP_C_ZLIB, PGP_C_BZIP2, PGP_C_ZIP};
        assert_true(prefs.z_algs == expected);
        // key server prefs
        expected = {PGP_KEY_SERVER_NO_MODIFY};
        assert_true(prefs.ks_prefs == expected);
        // preferred key server
        assert_true(prefs.key_server.empty());
    }

    /* Cleanup */
    delete pubring;
}
