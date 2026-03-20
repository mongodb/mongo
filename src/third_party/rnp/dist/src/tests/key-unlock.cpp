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

#include "../librepgp/stream-ctx.h"
#include "key.hpp"
#include "ffi-priv-types.h"
#include "rnp_tests.h"
#include "support.h"
#include <fstream>

TEST_F(rnp_tests, test_key_unlock_pgp)
{
    cli_rnp_t               rnp = {};
    const char *            data = "my test data";
    pgp_password_provider_t provider = {0};
    static const char *     keyids[] = {"7bc6709b15c23a4a", // primary
                                   "1ed63ee56fadc34d",
                                   "1d7e8a5393c997a8",
                                   "8a05b89fad5aded1",
                                   "2fcadf05ffa501bb", // primary
                                   "54505a936a4a970e",
                                   "326ef111425d14a5"};

    assert_true(setup_cli_rnp_common(&rnp, RNP_KEYSTORE_GPG, "data/keyrings/1/", NULL));
    assert_true(rnp.load_keyrings(true));

    for (size_t i = 0; i < ARRAY_SIZE(keyids); i++) {
        rnp_key_handle_t handle = NULL;
        assert_rnp_success(rnp_locate_key(rnp.ffi, "keyid", keyids[i], &handle));
        assert_non_null(handle);
        bool locked = false;
        assert_rnp_success(rnp_key_is_locked(handle, &locked));
        // all keys in this keyring are encrypted and thus should be locked initially
        assert_true(locked);
        rnp_key_handle_destroy(handle);
    }

    std::ofstream out("dummyfile.dat");
    out << data;
    out.close();

    // try signing with a failing password provider (should fail)
    assert_rnp_success(
      rnp_ffi_set_pass_provider(rnp.ffi, ffi_failing_password_provider, NULL));
    rnp_cfg &cfg = rnp.cfg();
    cfg.load_defaults();
    cfg.set_bool(CFG_SIGN_NEEDED, true);
    cfg.set_str(CFG_HASH, "SHA256");
    cfg.set_int(CFG_ZLEVEL, 0);
    cfg.set_str(CFG_INFILE, "dummyfile.dat");
    cfg.set_str(CFG_OUTFILE, "dummyfile.dat.pgp");
    cfg.add_str(CFG_SIGNERS, keyids[0]);
    assert_false(cli_rnp_protect_file(&rnp));

    // grab the signing key to unlock
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(rnp.ffi, "keyid", keyids[0], &key));
    assert_non_null(key);
    char *alg = NULL;
    // confirm that this key is indeed RSA first
    assert_rnp_success(rnp_key_get_alg(key, &alg));
    assert_int_equal(strcmp(alg, "RSA"), 0);
    rnp_buffer_destroy(alg);

    // confirm the secret MPIs are NULL
    assert_true(rsa_sec_empty(*key->sec->material()));

    // try to unlock with a failing password provider
    provider.callback = failing_password_callback;
    provider.userdata = NULL;
    assert_false(key->sec->unlock(provider));
    bool locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);

    // try to unlock with an incorrect password
    provider.callback = string_copy_password_callback;
    provider.userdata = (void *) "badpass";
    assert_false(key->sec->unlock(provider));
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);

    // unlock the signing key
    provider.callback = string_copy_password_callback;
    provider.userdata = (void *) "password";
    assert_true(key->sec->unlock(provider));
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_false(locked);

    // confirm the secret MPIs are now filled in
    assert_true(rsa_sec_filled(*key->sec->material()));

    // now the signing key is unlocked, confirm that no password is required for signing
    assert_rnp_success(
      rnp_ffi_set_pass_provider(rnp.ffi, ffi_asserting_password_provider, NULL));
    cfg.clear();
    cfg.load_defaults();
    cfg.set_bool(CFG_SIGN_NEEDED, true);
    cfg.set_str(CFG_HASH, "SHA256");
    cfg.set_int(CFG_ZLEVEL, 0);
    cfg.set_str(CFG_INFILE, "dummyfile.dat");
    cfg.set_str(CFG_OUTFILE, "dummyfile.dat.pgp");
    cfg.add_str(CFG_SIGNERS, keyids[0]);
    assert_true(cli_rnp_protect_file(&rnp));
    cfg.clear();

    // verify
    cfg.load_defaults();
    cfg.set_bool(CFG_OVERWRITE, true);
    cfg.set_str(CFG_INFILE, "dummyfile.dat.pgp");
    cfg.set_str(CFG_OUTFILE, "dummyfile.verify");
    assert_true(cli_rnp_process_file(&rnp));

    // verify (negative)
    std::fstream verf("dummyfile.dat.pgp",
                      std::ios_base::binary | std::ios_base::out | std::ios_base::in);
    verf.seekg(-3, std::ios::end);
    char bt = verf.peek() ^ 0xff;
    verf.seekp(-3, std::ios::end);
    verf.write(&bt, 1);
    verf.close();
    assert_false(cli_rnp_process_file(&rnp));

    // lock the signing key
    assert_rnp_success(rnp_key_lock(key));
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);

    // sign, with no password (should now fail)
    assert_rnp_success(
      rnp_ffi_set_pass_provider(rnp.ffi, ffi_failing_password_provider, NULL));
    cfg.clear();
    cfg.load_defaults();
    cfg.set_bool(CFG_SIGN_NEEDED, true);
    cfg.set_bool(CFG_OVERWRITE, true);
    cfg.set_str(CFG_HASH, "SHA1");
    cfg.set_int(CFG_ZLEVEL, 0);
    cfg.set_str(CFG_INFILE, "dummyfile.dat");
    cfg.set_str(CFG_OUTFILE, "dummyfile.dat.pgp");
    cfg.add_str(CFG_SIGNERS, keyids[0]);
    assert_false(cli_rnp_protect_file(&rnp));
    cfg.clear();

    // encrypt
    cfg.load_defaults();
    cfg.set_bool(CFG_ENCRYPT_PK, true);
    cfg.set_int(CFG_ZLEVEL, 0);
    cfg.set_str(CFG_INFILE, "dummyfile.dat");
    cfg.set_str(CFG_OUTFILE, "dummyfile.dat.pgp");
    cfg.set_bool(CFG_OVERWRITE, true);
    cfg.set_str(CFG_CIPHER, "AES256");
    cfg.add_str(CFG_RECIPIENTS, keyids[1]);
    assert_true(cli_rnp_protect_file(&rnp));
    cfg.clear();

    // try decrypting with a failing password provider (should fail)
    cfg.load_defaults();
    cfg.set_bool(CFG_OVERWRITE, true);
    cfg.set_str(CFG_INFILE, "dummyfile.dat.pgp");
    cfg.set_str(CFG_OUTFILE, "dummyfile.decrypt");
    assert_false(cli_rnp_process_file(&rnp));

    // grab the encrypting key to unlock
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_locate_key(rnp.ffi, "keyid", keyids[1], &subkey));
    assert_non_null(subkey);

    // unlock the encrypting key
    assert_rnp_success(rnp_key_unlock(subkey, "password"));
    assert_rnp_success(rnp_key_is_locked(subkey, &locked));
    assert_false(locked);

    // decrypt, with no password
    assert_true(cli_rnp_process_file(&rnp));

    std::string decrypt = file_to_str("dummyfile.decrypt");
    assert_true(decrypt == data);

    // lock the encrypting key
    assert_rnp_success(rnp_key_lock(subkey));
    assert_rnp_success(rnp_key_is_locked(subkey, &locked));
    assert_true(locked);

    // decrypt, with no password (should now fail)
    assert_false(cli_rnp_process_file(&rnp));
    // cleanup
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    rnp.end();
    assert_int_equal(rnp_unlink("dummyfile.dat"), 0);
}
