/*
 * Copyright (c) 2020 [Ribose Inc](https://www.ribose.com).
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

#include "../rnp_tests.h"
#include "../support.h"

static void
test_issue_1030(const char *keystore)
{
    int         pipefd[2] = {-1, -1};
    cli_rnp_t   rnp = {};
    const char *userid = "user";
    size_t      keycount = 0;

    char *home = make_temp_dir();
    assert_true(setup_cli_rnp_common(&rnp, keystore, home, pipefd));
    cli_set_default_rsa_key_desc(rnp.cfg(), "SHA256");
    assert_true(cli_rnp_generate_key(&rnp, userid));

    assert_true(rnp.load_keyrings(true));
    assert_rnp_success(rnp_get_public_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 2);
    assert_rnp_success(rnp_get_secret_key_count(rnp.ffi, &keycount));
    assert_int_equal(keycount, 2);

    std::vector<rnp_key_handle_t> keys;
    assert_true(rnp.keys_matching(keys, userid, CLI_SEARCH_SUBKEYS_AFTER));
    assert_int_equal(keys.size(), 2);
    clear_key_handles(keys);

    assert_true(rnp.keys_matching(keys, userid, CLI_SEARCH_SECRET | CLI_SEARCH_SUBKEYS_AFTER));
    assert_int_equal(keys.size(), 2);
    for (auto key : keys) {
        bool is_protected = false;
        assert_rnp_success(rnp_key_is_protected(key, &is_protected));
        assert_true(is_protected);
    }
    clear_key_handles(keys);

    // done
    if (pipefd[0] != -1) {
        close(pipefd[0]);
    }
    rnp.end();
    clean_temp_dir(home);
    free(home);
}

TEST_F(rnp_tests, issue_1030_rnpkeys_secret_keys_unprotected)
{
    for (auto keystore : {RNP_KEYSTORE_GPG, RNP_KEYSTORE_GPG21, RNP_KEYSTORE_KBX}) {
        test_issue_1030(keystore);
    }
}
