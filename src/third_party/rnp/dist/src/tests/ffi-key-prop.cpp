/*
 * Copyright (c) 2021 [Ribose Inc](https://www.ribose.com).
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

#include <sstream>
#include <rnp/rnp.h>
#include "rnp_tests.h"
#include "support.h"
#include <librepgp/stream-ctx.h>
#include "key.hpp"
#include "ffi-priv-types.h"

TEST_F(rnp_tests, test_ffi_key_set_expiry_multiple_uids)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    /* load key with 3 uids with zero key expiration */
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-3-uids.pgp"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    size_t count = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 3);
    uint32_t expiry = 10;
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    bool expired = true;
    assert_rnp_failure(rnp_key_is_expired(key, NULL));
    assert_rnp_failure(rnp_key_is_expired(NULL, &expired));
    rnp_key_handle_t bkey = bogus_key_handle(ffi);
    assert_non_null(bkey);
    assert_rnp_failure(rnp_key_is_expired(bkey, &expired));
    rnp_key_handle_destroy(bkey);
    assert_rnp_success(rnp_key_is_expired(key, &expired));
    assert_false(expired);
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_valid(key, 1, true));
    assert_true(check_uid_valid(key, 2, true));
    assert_true(check_uid_primary(key, 0, false));
    assert_true(check_uid_primary(key, 1, false));
    assert_true(check_uid_primary(key, 2, false));
    /* set expiration time to minimum value so key is expired now, but uids are still valid */
    assert_rnp_success(rnp_key_set_expiration(key, 1));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 1);
    assert_rnp_success(rnp_key_is_expired(key, &expired));
    assert_true(expired);
    bool valid = true;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_false(valid);
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_valid(key, 1, true));
    assert_true(check_uid_valid(key, 2, true));
    /* reload */
    rnp_key_handle_destroy(key);
    reload_keyrings(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 1);
    assert_rnp_success(rnp_key_is_expired(key, &expired));
    assert_true(expired);
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_valid(key, 1, true));
    assert_true(check_uid_valid(key, 2, true));
    /* set expiration to maximum value */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_key_set_expiration(key, 0xFFFFFFFF));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0xFFFFFFFF);
    valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_is_expired(key, &expired));
    assert_false(expired);
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_valid(key, 1, true));
    assert_true(check_uid_valid(key, 2, true));
    rnp_key_handle_destroy(key);
    /* reload and make sure changes are saved */
    reload_keyrings(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Caesar <caesar@rnp>", &key));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0xFFFFFFFF);
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_valid(key, 1, true));
    assert_true(check_uid_valid(key, 2, true));
    rnp_key_handle_destroy(key);

    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    /* load key with 3 uids, including primary, with key expiration */
    assert_true(
      import_all_keys(ffi, "data/test_key_edge_cases/alice-3-uids-primary-expiring.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    expiry = 0;
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 674700647);
    assert_rnp_success(rnp_key_is_expired(key, &expired));
    assert_false(expired);
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_valid(key, 1, true));
    assert_true(check_uid_valid(key, 2, true));
    assert_true(check_uid_primary(key, 0, true));
    assert_true(check_uid_primary(key, 1, false));
    assert_true(check_uid_primary(key, 2, false));
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_success(rnp_key_set_expiration(key, 0));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_rnp_success(rnp_key_is_expired(key, &expired));
    assert_false(expired);
    valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_valid(key, 1, true));
    assert_true(check_uid_valid(key, 2, true));
    rnp_key_handle_destroy(key);
    /* reload and make sure it is saved */
    reload_keyrings(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Caesar <caesar@rnp>", &key));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_valid(key, 1, true));
    assert_true(check_uid_valid(key, 2, true));
    assert_true(check_uid_primary(key, 0, true));
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_primary_uid_conflict)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    /* load key with 1 uid and two certifications: first marks uid primary, but expires key
     * second marks uid as non-primary, but has zero key expiration */
    assert_true(
      import_all_keys(ffi, "data/test_key_edge_cases/key-primary-uid-conflict-pub.pgp"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid_2_sigs", &key));
    assert_int_equal(get_key_uids(key), 1);
    assert_int_equal(get_key_expiry(key), 0);
    assert_true(check_key_valid(key, true));
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_primary(key, 0, false));
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_expired_certification_and_direct_sig)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    /* load key with 2 uids and direct-key signature:
     * - direct-key sig has 0 key expiration time but expires in 30 seconds
     * - first uid is not primary, but key expiration is 60 seconds
     * - second uid is marked as primary, doesn't expire key, but certification expires in 60
     *   seconds */
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/key-expired-cert-direct.pgp"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "primary-uid-expired-cert", &key));
    assert_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "expired-certifications", &key));
    assert_non_null(key);
    assert_int_equal(get_key_uids(key), 2);
    assert_int_equal(get_key_expiry(key), 60);
    rnp_signature_handle_t sig = NULL;
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &sig));
    assert_non_null(sig);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_EXPIRED);
    size_t errors = 0;
    assert_int_equal(rnp_signature_error_count(NULL, &errors), RNP_ERROR_NULL_POINTER);
    assert_int_equal(rnp_signature_error_count(sig, NULL), RNP_ERROR_NULL_POINTER);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 1);
    uint32_t error = 0;
    assert_int_equal(rnp_signature_error_at(NULL, 0, &error), RNP_ERROR_NULL_POINTER);
    assert_int_equal(rnp_signature_error_at(sig, 0, NULL), RNP_ERROR_NULL_POINTER);
    assert_int_equal(rnp_signature_error_at(sig, 1, &error), RNP_ERROR_BAD_PARAMETERS);
    assert_int_equal(rnp_signature_error_at(sig, 10000, &error), RNP_ERROR_BAD_PARAMETERS);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_EXPIRED);
    rnp_signature_handle_destroy(sig);
    assert_true(check_key_valid(key, false));
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_uid_primary(key, 0, false));
    assert_true(check_uid_valid(key, 1, false));
    assert_true(check_uid_primary(key, 1, false));
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_25519_tweaked_bits)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* try public key */
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/key-25519-non-tweaked.asc"));
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "950EE0CD34613DBA", &sub));
    bool tweaked = true;
    assert_rnp_failure(rnp_key_25519_bits_tweaked(NULL, &tweaked));
    assert_rnp_failure(rnp_key_25519_bits_tweaked(sub, NULL));
    assert_rnp_failure(rnp_key_25519_bits_tweaked(sub, &tweaked));
    assert_rnp_failure(rnp_key_25519_bits_tweak(NULL));
    assert_rnp_failure(rnp_key_25519_bits_tweak(sub));
    rnp_key_handle_destroy(sub);
    /* load secret key */
    assert_true(
      import_all_keys(ffi, "data/test_key_edge_cases/key-25519-non-tweaked-sec.asc"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "950EE0CD34613DBA", &sub));
    assert_rnp_failure(rnp_key_25519_bits_tweaked(NULL, &tweaked));
    assert_rnp_failure(rnp_key_25519_bits_tweaked(sub, NULL));
    assert_rnp_success(rnp_key_25519_bits_tweaked(sub, &tweaked));
    assert_false(tweaked);
    /* protect key and try again */
    assert_rnp_success(rnp_key_protect(sub, "password", NULL, NULL, NULL, 100000));
    assert_rnp_failure(rnp_key_25519_bits_tweaked(sub, &tweaked));
    assert_rnp_success(rnp_key_unlock(sub, "password"));
    tweaked = true;
    assert_rnp_success(rnp_key_25519_bits_tweaked(sub, &tweaked));
    assert_false(tweaked);
    assert_rnp_success(rnp_key_lock(sub));
    assert_rnp_failure(rnp_key_25519_bits_tweaked(sub, &tweaked));
    /* now let's tweak it */
    assert_rnp_failure(rnp_key_25519_bits_tweak(NULL));
    assert_rnp_failure(rnp_key_25519_bits_tweak(sub));
    assert_rnp_success(rnp_key_unlock(sub, "password"));
    assert_rnp_failure(rnp_key_25519_bits_tweak(sub));
    assert_rnp_success(rnp_key_unprotect(sub, "password"));
    assert_rnp_success(rnp_key_25519_bits_tweak(sub));
    assert_rnp_success(rnp_key_25519_bits_tweaked(sub, &tweaked));
    assert_true(tweaked);
    /* export unprotected key */
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "3176FC1486AA2528", &key));
    auto clearsecdata = export_key(key, true, true);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_key_protect(sub, "password", NULL, NULL, NULL, 100000));
    rnp_key_handle_destroy(sub);
    /* make sure it is exported and saved tweaked and protected */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "3176FC1486AA2528", &key));
    auto secdata = export_key(key, true, true);
    rnp_key_handle_destroy(key);
    reload_keyrings(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "950EE0CD34613DBA", &sub));
    bool prot = false;
    assert_rnp_success(rnp_key_is_protected(sub, &prot));
    assert_true(prot);
    assert_rnp_success(rnp_key_unlock(sub, "password"));
    tweaked = false;
    assert_rnp_success(rnp_key_25519_bits_tweaked(sub, &tweaked));
    assert_true(tweaked);
    rnp_key_handle_destroy(sub);
    rnp_ffi_destroy(ffi);
    /* import cleartext exported key */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, clearsecdata.data(), clearsecdata.size()));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "950EE0CD34613DBA", &sub));
    prot = true;
    assert_rnp_success(rnp_key_is_protected(sub, &prot));
    assert_false(prot);
    tweaked = false;
    assert_rnp_success(rnp_key_25519_bits_tweaked(sub, &tweaked));
    assert_true(tweaked);
    rnp_key_handle_destroy(sub);
    rnp_ffi_destroy(ffi);
    /* import exported key */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, secdata.data(), secdata.size()));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "950EE0CD34613DBA", &sub));
    prot = false;
    assert_rnp_success(rnp_key_is_protected(sub, &prot));
    assert_true(prot);
    assert_rnp_success(rnp_key_unlock(sub, "password"));
    tweaked = false;
    assert_rnp_success(rnp_key_25519_bits_tweaked(sub, &tweaked));
    assert_true(tweaked);
    rnp_key_handle_destroy(sub);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_revoke)
{
    rnp_ffi_t ffi = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sub-pub.pgp"));
    rnp_key_handle_t key_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    /* check for failure with wrong parameters */
    assert_rnp_failure(rnp_key_revoke(NULL, 0, "SHA256", "superseded", "test key revocation"));
    assert_rnp_failure(rnp_key_revoke(key_handle, 0, "SHA256", NULL, NULL));
    assert_rnp_failure(rnp_key_revoke(key_handle, 0x17, "SHA256", NULL, NULL));
    assert_rnp_failure(rnp_key_revoke(key_handle, 0, "Wrong hash", NULL, NULL));
    assert_rnp_failure(rnp_key_revoke(key_handle, 0, "SHA256", "Wrong reason code", NULL));
    /* attempt to revoke key without the secret */
    assert_rnp_failure(rnp_key_revoke(key_handle, 0, "SHA256", "retired", "Custom reason"));
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    /* attempt to revoke subkey without the secret */
    rnp_key_handle_t sub_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub_handle));
    assert_rnp_failure(rnp_key_revoke(sub_handle, 0, "SHA256", "retired", "Custom reason"));
    assert_rnp_success(rnp_key_handle_destroy(sub_handle));
    /* load secret key */
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub_handle));
    /* wrong password - must fail */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "wrong"));
    assert_rnp_failure(rnp_key_revoke(key_handle, 0, "SHA256", "superseded", NULL));
    assert_rnp_failure(rnp_key_revoke(sub_handle, 0, "SHA256", "superseded", NULL));
    /* unlocked key - must succeed */
    bool revoked = false;
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_false(revoked);
    assert_rnp_success(rnp_key_unlock(key_handle, "password"));
    assert_rnp_success(rnp_key_revoke(key_handle, 0, "SHA256", NULL, NULL));
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    /* subkey */
    assert_rnp_success(rnp_key_is_revoked(sub_handle, &revoked));
    assert_false(revoked);
    bool locked = true;
    assert_rnp_success(rnp_key_is_locked(key_handle, &locked));
    assert_false(locked);
    assert_rnp_success(rnp_key_revoke(sub_handle, 0, "SHA256", NULL, "subkey revoked"));
    assert_rnp_success(rnp_key_is_revoked(sub_handle, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_lock(key_handle));
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    assert_rnp_success(rnp_key_handle_destroy(sub_handle));
    /* correct password provider - must succeed */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_SECRET | RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_false(revoked);
    assert_rnp_success(
      rnp_key_revoke(key_handle, 0, "SHA256", "superseded", "test key revocation"));
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    /* make sure FFI locks key back */
    assert_rnp_success(rnp_key_is_locked(key_handle, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    /* repeat for subkey */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub_handle));
    assert_rnp_success(rnp_key_is_revoked(sub_handle, &revoked));
    assert_false(revoked);
    assert_rnp_success(rnp_key_revoke(sub_handle, 0, "SHA256", "no", "test sub revocation"));
    assert_rnp_success(rnp_key_is_revoked(sub_handle, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_handle_destroy(sub_handle));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_set_expiry)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sub-pub.pgp"));

    /* check edge cases */
    assert_rnp_failure(rnp_key_set_expiration(NULL, 0));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    /* cannot set key expiration with public key only */
    assert_rnp_failure(rnp_key_set_expiration(key, 1000));
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_failure(rnp_key_set_expiration(sub, 1000));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    /* load secret key */
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sub-sec.pgp"));
    uint32_t       expiry = 0;
    const uint32_t new_expiry = 10 * 365 * 24 * 60 * 60;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    expiry = 255;
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_rnp_success(rnp_key_set_expiration(key, 0));
    /* will fail on locked key */
    assert_rnp_failure(rnp_key_set_expiration(key, new_expiry));
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_success(rnp_key_set_expiration(key, new_expiry));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    /* will succeed on locked subkey since it is not signing one */
    assert_rnp_success(rnp_key_set_expiration(sub, 0));
    assert_rnp_success(rnp_key_set_expiration(sub, new_expiry * 2));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, new_expiry * 2);
    /* make sure new expiration times are properly saved */
    rnp_output_t keymem = NULL;
    rnp_output_t seckeymem = NULL;
    assert_rnp_success(rnp_output_to_memory(&keymem, 0));
    assert_rnp_success(
      rnp_key_export(key, keymem, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_output_to_memory(&seckeymem, 0));
    assert_rnp_success(
      rnp_key_export(key, seckeymem, RNP_KEY_EXPORT_SECRET | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    uint8_t *keybuf = NULL;
    size_t   keylen = 0;
    assert_rnp_success(rnp_output_memory_get_buf(keymem, &keybuf, &keylen, false));
    /* load public key */
    assert_true(import_pub_keys(ffi, keybuf, keylen));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, new_expiry * 2);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    /* now load exported secret key */
    assert_rnp_success(rnp_output_memory_get_buf(seckeymem, &keybuf, &keylen, false));
    assert_true(import_sec_keys(ffi, keybuf, keylen));
    assert_rnp_success(rnp_output_destroy(seckeymem));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, new_expiry * 2);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));
    /* now unset expiration time back, first loading the public key back */
    assert_rnp_success(rnp_output_memory_get_buf(keymem, &keybuf, &keylen, false));
    assert_true(import_pub_keys(ffi, keybuf, keylen));
    assert_rnp_success(rnp_output_destroy(keymem));
    /* set primary key expiration */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_success(rnp_key_set_expiration(key, 0));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_success(rnp_key_set_expiration(sub, 0));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 0);
    /* let's export them and reload */
    assert_rnp_success(rnp_output_to_memory(&keymem, 0));
    assert_rnp_success(
      rnp_key_export(key, keymem, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(rnp_output_memory_get_buf(keymem, &keybuf, &keylen, false));
    assert_true(import_pub_keys(ffi, keybuf, keylen));
    assert_rnp_success(rnp_output_destroy(keymem));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    bool expired = true;
    assert_rnp_success(rnp_key_is_expired(key, &expired));
    assert_false(expired);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 0);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    /* now try the sign-able subkey */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sign-sub-pub.pgp"));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sign-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "22F3A217C0E439CB", &sub));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 0);
    assert_rnp_failure(rnp_key_set_expiration(sub, new_expiry));
    /* now unlock only primary key - should fail */
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_failure(rnp_key_set_expiration(sub, new_expiry));
    /* unlock subkey */
    assert_rnp_success(rnp_key_unlock(sub, "password"));
    assert_rnp_success(rnp_key_set_expiration(sub, new_expiry));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_rnp_success(rnp_output_to_memory(&keymem, 0));
    assert_rnp_success(
      rnp_key_export(key, keymem, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(rnp_output_memory_get_buf(keymem, &keybuf, &keylen, false));
    assert_true(import_pub_keys(ffi, keybuf, keylen));
    assert_rnp_success(rnp_output_destroy(keymem));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "22F3A217C0E439CB", &sub));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_rnp_success(rnp_key_handle_destroy(sub));

    /* check whether we can change expiration for already expired key */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sign-sub-pub.pgp"));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sign-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "22F3A217C0E439CB", &sub));
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_success(rnp_key_unlock(sub, "password"));
    assert_rnp_success(rnp_key_set_expiration(key, 1));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 1);
    assert_rnp_success(rnp_key_is_expired(key, &expired));
    assert_true(expired);

    /* key is invalid since it is expired */
    assert_false(key->pub->valid());
    bool valid = true;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_false(valid);
    uint32_t till = 0;
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, 1577369391 + 1);
    uint64_t till64 = 0;
    assert_rnp_success(rnp_key_valid_till64(key, &till64));
    assert_int_equal(till64, 1577369391 + 1);
    assert_rnp_success(rnp_key_set_expiration(sub, 1));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 1);
    assert_false(sub->pub->valid());
    valid = true;
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_false(valid);
    till = 1;
    assert_rnp_success(rnp_key_valid_till(sub, &till));
    assert_int_equal(till, 1577369391 + 1);
    assert_rnp_success(rnp_key_valid_till64(sub, &till64));
    assert_int_equal(till64, 1577369391 + 1);
    assert_rnp_success(rnp_key_set_expiration(key, 0));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_true(key->pub->valid());
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, 0xffffffff);
    assert_rnp_success(rnp_key_valid_till64(key, &till64));
    assert_int_equal(till64, UINT64_MAX);
    assert_rnp_success(rnp_key_set_expiration(sub, 0));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 0);
    assert_true(sub->pub->valid());
    valid = false;
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    till = 0;
    assert_rnp_success(rnp_key_valid_till(sub, &till));
    assert_int_equal(till, 0xffffffff);
    till64 = 0;
    assert_rnp_success(rnp_key_valid_till64(sub, &till64));
    assert_int_equal(till64, UINT64_MAX);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    /* check whether we can change expiration with password provider/locked key */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sign-sub-pub.pgp"));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sign-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "22F3A217C0E439CB", &sub));

    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "wrong"));
    assert_rnp_failure(rnp_key_set_expiration(key, 1));
    expiry = 255;
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_rnp_failure(rnp_key_set_expiration(sub, 1));
    expiry = 255;
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 0);

    bool locked = true;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    uint32_t creation = 0;
    assert_rnp_success(rnp_key_get_creation(key, &creation));
    creation = time(NULL) - creation;
    assert_rnp_success(rnp_key_set_expiration(key, creation + 8));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, creation + 8);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_creation(sub, &creation));
    creation = time(NULL) - creation;
    assert_rnp_success(rnp_key_set_expiration(sub, creation + 3));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, creation + 3);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);

    /* now change just subkey's expiration - should also work */
    valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_set_expiration(sub, 4));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 4);
    assert_rnp_success(rnp_key_is_expired(sub, &expired));
    assert_true(expired);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);

    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    /* now try to update already expired key and subkey */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sign-sub-exp-pub.asc"));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sign-sub-exp-sec.asc"));
    /* Alice key is searchable by userid since self-sig is not expired, and it just marks key
     * as expired */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "22F3A217C0E439CB", &sub));
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    /* key is not valid since expired */
    assert_false(valid);
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, 1577369391 + 16324055);
    assert_rnp_success(rnp_key_valid_till64(key, &till64));
    assert_int_equal(till64, 1577369391 + 16324055);
    assert_false(key->pub->valid());
    /* secret key part is also not valid till new sig is added */
    assert_false(key->sec->valid());
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_false(valid);
    assert_rnp_success(rnp_key_valid_till(sub, &till));
    /* subkey valid no longer then the primary key */
    assert_int_equal(till, 1577369391 + 16324055);
    assert_rnp_success(rnp_key_valid_till64(sub, &till64));
    assert_int_equal(till64, 1577369391 + 16324055);
    assert_false(sub->pub->valid());
    assert_false(sub->sec->valid());
    creation = 0;
    uint32_t validity = 2 * 30 * 24 * 60 * 60; // 2 months
    assert_rnp_success(rnp_key_get_creation(key, &creation));
    uint32_t keytill = creation + validity;
    creation = time(NULL) - creation;
    keytill += creation;
    assert_rnp_success(rnp_key_set_expiration(key, creation + validity));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, creation + validity);
    assert_rnp_success(rnp_key_get_creation(sub, &creation));
    /* use smaller validity for the subkey */
    validity = validity / 2;
    uint32_t subtill = creation + validity;
    creation = time(NULL) - creation;
    subtill += creation;
    assert_rnp_success(rnp_key_set_expiration(sub, creation + validity));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, creation + validity);
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, keytill);
    assert_rnp_success(rnp_key_valid_till64(key, &till64));
    assert_int_equal(till64, keytill);
    assert_true(key->pub->valid());
    assert_true(key->sec->valid());
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_valid_till(sub, &till));
    assert_int_equal(till, subtill);
    assert_rnp_success(rnp_key_valid_till64(sub, &till64));
    assert_int_equal(till64, subtill);
    assert_true(sub->pub->valid());
    assert_true(sub->sec->valid());
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    /* update expiration time when only secret key is available */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "22F3A217C0E439CB", &sub));
    validity = 30 * 24 * 60 * 60; // 1 month
    assert_rnp_success(rnp_key_get_creation(key, &creation));
    creation = time(NULL) - creation;
    assert_rnp_success(rnp_key_set_expiration(key, creation + validity));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, creation + validity);
    assert_rnp_success(rnp_key_get_creation(sub, &creation));
    creation = time(NULL) - creation;
    assert_rnp_success(rnp_key_set_expiration(sub, creation + validity));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, creation + validity);
    /* public key is not available - bad parameters */
    assert_int_equal(rnp_key_is_valid(key, &valid), RNP_ERROR_BAD_PARAMETERS);
    assert_int_equal(rnp_key_valid_till(key, &till), RNP_ERROR_BAD_PARAMETERS);
    assert_int_equal(rnp_key_valid_till64(key, &till64), RNP_ERROR_BAD_PARAMETERS);
    assert_null(key->pub);
    assert_true(key->sec->valid());
    assert_int_equal(rnp_key_is_valid(sub, &valid), RNP_ERROR_BAD_PARAMETERS);
    assert_int_equal(rnp_key_valid_till(sub, &till), RNP_ERROR_BAD_PARAMETERS);
    assert_int_equal(rnp_key_valid_till64(sub, &till64), RNP_ERROR_BAD_PARAMETERS);
    assert_null(sub->pub);
    assert_true(sub->sec->valid());
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));
    assert_rnp_success(rnp_ffi_destroy(ffi));

    /* check whether things work for G10 keyring */
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_true(load_keys_kbx_g10(
      ffi, "data/keyrings/3/pubring.kbx", "data/keyrings/3/private-keys-v1.d"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "4BE147BB22DF1E60", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "A49BAE05C16E8BC8", &sub));
    assert_rnp_success(rnp_key_get_creation(key, &creation));
    keytill = creation + validity;
    creation = time(NULL) - creation;
    keytill += creation;
    assert_rnp_success(rnp_key_set_expiration(key, creation + validity));
    expiry = 255;
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, creation + validity);
    size_t key_expiry = expiry;
    assert_rnp_success(rnp_key_get_creation(sub, &creation));
    creation = time(NULL) - creation;
    assert_rnp_success(rnp_key_set_expiration(sub, creation + validity));
    expiry = 255;
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, creation + validity);
    size_t sub_expiry = expiry;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, keytill);
    assert_rnp_success(rnp_key_valid_till64(key, &till64));
    assert_int_equal(till64, keytill);
    assert_true(key->pub->valid());
    assert_true(key->sec->valid());
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_valid_till(sub, &till));
    assert_int_equal(till, keytill);
    assert_rnp_success(rnp_key_valid_till64(sub, &till64));
    assert_int_equal(till64, keytill);
    assert_true(sub->pub->valid());
    assert_true(sub->sec->valid());
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    /* save keyring to KBX and reload it: fails now */
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_path(&output, "pubring.kbx"));
    assert_rnp_success(rnp_save_keys(ffi, "KBX", output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_ffi_destroy(ffi));
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    assert_rnp_success(rnp_input_from_path(&input, "pubring.kbx"));
    /* Saving to KBX doesn't work well, or was broken at some point. */
    assert_rnp_failure(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "4BE147BB22DF1E60", &key));
    assert_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "A49BAE05C16E8BC8", &sub));
    assert_null(sub);
    expiry = 255;
    assert_rnp_failure(rnp_key_get_expiration(key, &expiry));
    assert_int_not_equal(expiry, key_expiry);
    expiry = 255;
    assert_rnp_failure(rnp_key_get_expiration(sub, &expiry));
    assert_int_not_equal(expiry, sub_expiry);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));
    assert_int_equal(rnp_unlink("pubring.kbx"), 0);
    assert_rnp_success(rnp_ffi_destroy(ffi));

    /* load G10/KBX and unload public keys - must succeed */
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_true(load_keys_kbx_g10(
      ffi, "data/keyrings/3/pubring.kbx", "data/keyrings/3/private-keys-v1.d"));
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "4BE147BB22DF1E60", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "A49BAE05C16E8BC8", &sub));
    assert_rnp_success(rnp_key_get_creation(key, &creation));
    creation = time(NULL) - creation;
    assert_rnp_success(rnp_key_set_expiration(key, creation + validity));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, creation + validity);
    key_expiry = expiry;
    assert_rnp_success(rnp_key_get_creation(sub, &creation));
    creation = time(NULL) - creation;
    assert_rnp_success(rnp_key_set_expiration(sub, creation + validity));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, creation + validity);
    sub_expiry = expiry;
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    // TODO: check expiration date in direct-key signature, check without
    // self-signature/binding signature.

    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_upgrade_hash_on_set_expiry)
{
    rnp_ffi_t ffi = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    assert_true(import_pub_keys(ffi, "data/keyrings/7/pubring.gpg"));
    assert_true(import_sec_keys(ffi, "data/keyrings/7/secring.gpg"));

    rnp_key_handle_t key = NULL;
    rnp_key_handle_t sub = NULL;
    uint32_t         expiry = 0;
    const uint32_t   new_expiry = 10 * 365 * 24 * 60 * 60;

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "07f90cc9ea074d53", &key));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_int_equal(key->pub->get_sig(0).sig.halg, PGP_HASH_SHA1);
    assert_rnp_success(rnp_key_set_expiration(key, new_expiry));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_int_equal(key->pub->get_sig(0).sig.halg, PGP_HASH_SHA256);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0265f8e2594f8e7b", &sub));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 0);
    assert_int_equal(sub->pub->get_sig(0).sig.halg, PGP_HASH_SHA1);
    assert_rnp_success(rnp_key_set_expiration(sub, new_expiry));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_int_equal(sub->pub->get_sig(0).sig.halg, PGP_HASH_SHA256);

    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    rnp_ffi_destroy(ffi);

    /* allow SHA1 and check that hash alg is not changed */

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    // Allow SHA1
    auto     now = time(NULL);
    uint64_t from = 0;
    uint32_t level = 0;
    rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "SHA1", now, NULL, &from, &level);
    rnp_add_security_rule(ffi,
                          RNP_FEATURE_HASH_ALG,
                          "SHA1",
                          RNP_SECURITY_OVERRIDE | RNP_SECURITY_VERIFY_KEY,
                          from,
                          RNP_SECURITY_DEFAULT);

    assert_true(import_pub_keys(ffi, "data/keyrings/7/pubring.gpg"));
    assert_true(import_sec_keys(ffi, "data/keyrings/7/secring.gpg"));

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "07f90cc9ea074d53", &key));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_int_equal(key->pub->get_sig(0).sig.halg, PGP_HASH_SHA1);
    assert_rnp_success(rnp_key_set_expiration(key, new_expiry));
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_int_equal(key->pub->get_sig(0).sig.halg, PGP_HASH_SHA1);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0265f8e2594f8e7b", &sub));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 0);
    assert_int_equal(sub->pub->get_sig(0).sig.halg, PGP_HASH_SHA1);
    assert_rnp_success(rnp_key_set_expiration(sub, new_expiry));
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, new_expiry);
    assert_int_equal(sub->pub->get_sig(0).sig.halg, PGP_HASH_SHA1);

    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(sub));

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_get_protection_info)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* Edge cases - public key, NULL parameters, etc. */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sub-pub.pgp"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    char *type = NULL;
    assert_rnp_failure(rnp_key_get_protection_type(key, NULL));
    assert_rnp_failure(rnp_key_get_protection_type(NULL, &type));
    assert_rnp_failure(rnp_key_get_protection_type(key, &type));
    char *mode = NULL;
    assert_rnp_failure(rnp_key_get_protection_mode(key, NULL));
    assert_rnp_failure(rnp_key_get_protection_mode(NULL, &mode));
    assert_rnp_failure(rnp_key_get_protection_mode(key, &mode));
    char *cipher = NULL;
    assert_rnp_failure(rnp_key_get_protection_cipher(key, NULL));
    assert_rnp_failure(rnp_key_get_protection_cipher(NULL, &cipher));
    assert_rnp_failure(rnp_key_get_protection_cipher(key, &cipher));
    char *hash = NULL;
    assert_rnp_failure(rnp_key_get_protection_hash(key, NULL));
    assert_rnp_failure(rnp_key_get_protection_hash(NULL, &hash));
    assert_rnp_failure(rnp_key_get_protection_hash(key, &hash));
    size_t iterations = 0;
    assert_rnp_failure(rnp_key_get_protection_iterations(key, NULL));
    assert_rnp_failure(rnp_key_get_protection_iterations(NULL, &iterations));
    assert_rnp_failure(rnp_key_get_protection_iterations(key, &iterations));
    rnp_key_handle_destroy(key);

    /* Encrypted secret key with subkeys */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_all_keys(ffi, "data/test_key_validity/alice-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "CFB");
    rnp_buffer_destroy(mode);
    assert_rnp_success(rnp_key_get_protection_cipher(key, &cipher));
    assert_string_equal(cipher, "AES128");
    rnp_buffer_destroy(cipher);
    assert_rnp_success(rnp_key_get_protection_hash(key, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    assert_rnp_success(rnp_key_get_protection_iterations(key, &iterations));
    assert_int_equal(iterations, 22020096);
    assert_rnp_success(rnp_key_unprotect(key, "password"));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "None");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "None");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(key, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(key, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(key, &iterations));
    rnp_key_handle_destroy(key);

    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(sub, &mode));
    assert_string_equal(mode, "CFB");
    rnp_buffer_destroy(mode);
    assert_rnp_success(rnp_key_get_protection_cipher(sub, &cipher));
    assert_string_equal(cipher, "AES128");
    rnp_buffer_destroy(cipher);
    assert_rnp_success(rnp_key_get_protection_hash(sub, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    assert_rnp_success(rnp_key_get_protection_iterations(sub, &iterations));
    assert_int_equal(iterations, 22020096);
    assert_rnp_success(rnp_key_unprotect(sub, "password"));
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "None");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(sub, &mode));
    assert_string_equal(mode, "None");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(sub, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(sub, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(sub, &iterations));
    rnp_key_handle_destroy(sub);

    /* v3 secret key */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/keyrings/4/pubring.pgp"));
    assert_true(import_sec_keys(ffi, "data/keyrings/4/secring.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7D0BC10E933404C9", &key));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Encrypted");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "CFB");
    rnp_buffer_destroy(mode);
    assert_rnp_success(rnp_key_get_protection_cipher(key, &cipher));
    assert_string_equal(cipher, "IDEA");
    rnp_buffer_destroy(cipher);
    assert_rnp_success(rnp_key_get_protection_hash(key, &hash));
    assert_string_equal(hash, "MD5");
    rnp_buffer_destroy(hash);
    assert_rnp_success(rnp_key_get_protection_iterations(key, &iterations));
    assert_int_equal(iterations, 1);
    if (idea_enabled()) {
        assert_rnp_success(rnp_key_unprotect(key, "password"));
        assert_rnp_success(rnp_key_get_protection_type(key, &type));
        assert_string_equal(type, "None");
        rnp_buffer_destroy(type);
        assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
        assert_string_equal(mode, "None");
        rnp_buffer_destroy(mode);
        assert_rnp_failure(rnp_key_get_protection_cipher(key, &cipher));
        assert_rnp_failure(rnp_key_get_protection_hash(key, &hash));
        assert_rnp_failure(rnp_key_get_protection_iterations(key, &iterations));
    } else {
        assert_rnp_failure(rnp_key_unprotect(key, "password"));
        assert_rnp_success(rnp_key_get_protection_type(key, &type));
        assert_string_equal(type, "Encrypted");
        rnp_buffer_destroy(type);
        assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
        assert_string_equal(mode, "CFB");
        rnp_buffer_destroy(mode);
    }
    rnp_key_handle_destroy(key);

    /* G10 keys */
    rnp_ffi_destroy(ffi);
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));

    assert_true(load_keys_kbx_g10(
      ffi, "data/keyrings/3/pubring.kbx", "data/keyrings/3/private-keys-v1.d"));

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "4BE147BB22DF1E60", &key));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "CBC");
    rnp_buffer_destroy(mode);
    assert_rnp_success(rnp_key_get_protection_cipher(key, &cipher));
    assert_string_equal(cipher, "AES128");
    rnp_buffer_destroy(cipher);
    assert_rnp_success(rnp_key_get_protection_hash(key, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    assert_rnp_success(rnp_key_get_protection_iterations(key, &iterations));
    assert_int_equal(iterations, 1024);
    assert_rnp_success(rnp_key_unprotect(key, "password"));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "None");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "None");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(key, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(key, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(key, &iterations));
    rnp_key_handle_destroy(key);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "A49BAE05C16E8BC8", &sub));
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(sub, &mode));
    assert_string_equal(mode, "CBC");
    rnp_buffer_destroy(mode);
    assert_rnp_success(rnp_key_get_protection_cipher(sub, &cipher));
    assert_string_equal(cipher, "AES128");
    rnp_buffer_destroy(cipher);
    assert_rnp_success(rnp_key_get_protection_hash(sub, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    assert_rnp_success(rnp_key_get_protection_iterations(sub, &iterations));
    assert_int_equal(iterations, 1024);
    assert_rnp_success(rnp_key_unprotect(sub, "password"));
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "None");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(sub, &mode));
    assert_string_equal(mode, "None");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(sub, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(sub, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(sub, &iterations));
    rnp_key_handle_destroy(sub);

    /* Secret subkeys, exported via gpg --export-secret-subkeys (no primary secret key data) */
    rnp_ffi_destroy(ffi);
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-1-subs.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "GPG-None");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "Unknown");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(key, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(key, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(key, &iterations));
    rnp_key_handle_destroy(key);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(sub, &mode));
    assert_string_equal(mode, "CFB");
    rnp_buffer_destroy(mode);
    assert_rnp_success(rnp_key_get_protection_cipher(sub, &cipher));
    assert_string_equal(cipher, "AES128");
    rnp_buffer_destroy(cipher);
    assert_rnp_success(rnp_key_get_protection_hash(sub, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    assert_rnp_success(rnp_key_get_protection_iterations(sub, &iterations));
    assert_int_equal(iterations, 30408704);
    assert_rnp_success(rnp_key_unprotect(sub, "password"));
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "None");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(sub, &mode));
    assert_string_equal(mode, "None");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(sub, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(sub, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(sub, &iterations));
    rnp_key_handle_destroy(sub);

    /* secret subkey is available, but primary key is stored on the smartcard by gpg */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-2-card.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "GPG-Smartcard");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "Unknown");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(key, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(key, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(key, &iterations));
    rnp_key_handle_destroy(key);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(sub, &mode));
    assert_string_equal(mode, "CFB");
    rnp_buffer_destroy(mode);
    assert_rnp_success(rnp_key_get_protection_cipher(sub, &cipher));
    assert_string_equal(cipher, "AES128");
    rnp_buffer_destroy(cipher);
    assert_rnp_success(rnp_key_get_protection_hash(sub, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    assert_rnp_success(rnp_key_get_protection_iterations(sub, &iterations));
    assert_int_equal(iterations, 30408704);
    assert_rnp_success(rnp_key_unprotect(sub, "password"));
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "None");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(sub, &mode));
    assert_string_equal(mode, "None");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(sub, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(sub, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(sub, &iterations));
    rnp_key_handle_destroy(sub);

    /* primary key is stored with unknown gpg s2k */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-3.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Unknown");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "Unknown");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(key, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(key, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(key, &iterations));
    rnp_key_handle_destroy(key);

    /* primary key is stored with unknown s2k */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-unknown.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Unknown");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_protection_mode(key, &mode));
    assert_string_equal(mode, "Unknown");
    rnp_buffer_destroy(mode);
    assert_rnp_failure(rnp_key_get_protection_cipher(key, &cipher));
    assert_rnp_failure(rnp_key_get_protection_hash(key, &hash));
    assert_rnp_failure(rnp_key_get_protection_iterations(key, &iterations));
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_default_subkey)
{
    rnp_ffi_t        ffi = NULL;
    rnp_key_handle_t primary = NULL;
    rnp_key_handle_t def_key = NULL;
    char *           keyid = NULL;

    test_ffi_init(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &primary));

    /* bad parameters */
    assert_rnp_failure(rnp_key_get_default_key(NULL, NULL, 0, NULL));
    assert_rnp_failure(rnp_key_get_default_key(primary, NULL, 0, NULL));
    assert_rnp_failure(rnp_key_get_default_key(primary, "nonexistentusage", 0, &def_key));
    assert_rnp_failure(rnp_key_get_default_key(primary, "sign", UINT32_MAX, &def_key));
    assert_rnp_failure(rnp_key_get_default_key(primary, "sign", 0, NULL));

    assert_rnp_success(
      rnp_key_get_default_key(primary, "encrypt", RNP_KEY_SUBKEYS_ONLY, &def_key));
    assert_rnp_success(rnp_key_get_keyid(def_key, &keyid));
    assert_string_equal(keyid, "8A05B89FAD5ADED1");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(def_key);

    /* no signing subkey */
    assert_int_equal(RNP_ERROR_NO_SUITABLE_KEY,
                     rnp_key_get_default_key(primary, "sign", RNP_KEY_SUBKEYS_ONLY, &def_key));
    assert_null(def_key);

    /* primary key returned as a default one */
    assert_rnp_success(rnp_key_get_default_key(primary, "sign", 0, &def_key));
    assert_rnp_success(rnp_key_get_keyid(def_key, &keyid));
    assert_string_equal(keyid, "7BC6709B15C23A4A");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(def_key);

    assert_rnp_success(rnp_key_get_default_key(primary, "certify", 0, &def_key));
    assert_rnp_success(rnp_key_get_keyid(def_key, &keyid));
    assert_string_equal(keyid, "7BC6709B15C23A4A");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(def_key);

    rnp_key_handle_destroy(primary);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));

    /* primary key with encrypting capability */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/encrypting-primary.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "92091b7b76c50017", &primary));

    assert_rnp_success(rnp_key_get_default_key(primary, "encrypt", 0, &def_key));
    assert_rnp_success(rnp_key_get_keyid(def_key, &keyid));
    assert_string_equal(keyid, "92091B7B76C50017");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(def_key);

    assert_rnp_success(
      rnp_key_get_default_key(primary, "encrypt", RNP_KEY_SUBKEYS_ONLY, &def_key));
    assert_rnp_success(rnp_key_get_keyid(def_key, &keyid));
    assert_string_equal(keyid, "C2E243E872C1FE50");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(def_key);
    rnp_key_handle_destroy(primary);

    /* offline primary key - must select a subkey */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-1-subs.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &primary));
    def_key = NULL;
    assert_rnp_success(rnp_key_get_default_key(primary, "sign", 0, &def_key));
    assert_rnp_success(rnp_key_get_keyid(def_key, &keyid));
    assert_string_equal(keyid, "22F3A217C0E439CB");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(def_key);
    rnp_key_handle_destroy(primary);
    /* offline primary key, stored on card - must select a subkey */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-2-card.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &primary));
    def_key = NULL;
    assert_rnp_success(rnp_key_get_default_key(primary, "sign", 0, &def_key));
    assert_rnp_success(rnp_key_get_keyid(def_key, &keyid));
    assert_string_equal(keyid, "22F3A217C0E439CB");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(def_key);
    rnp_key_handle_destroy(primary);
    /* offline primary key without the signing subkey - fail */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(
      import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-no-sign-sub.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &primary));
    def_key = NULL;
    assert_int_equal(rnp_key_get_default_key(primary, "sign", 0, &def_key),
                     RNP_ERROR_NO_SUITABLE_KEY);
    rnp_key_handle_destroy(primary);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_rnp_key_get_primary_grip)
{
    rnp_ffi_t        ffi = NULL;
    rnp_key_handle_t key = NULL;
    char *           grip = NULL;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    // load our keyrings
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));

    // locate primary key
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7BC6709B15C23A4A", &key));
    assert_non_null(key);

    // some edge cases
    assert_rnp_failure(rnp_key_get_primary_grip(NULL, NULL));
    assert_rnp_failure(rnp_key_get_primary_grip(NULL, &grip));
    assert_rnp_failure(rnp_key_get_primary_grip(key, NULL));
    assert_rnp_failure(rnp_key_get_primary_grip(key, &grip));
    assert_null(grip);
    rnp_key_handle_destroy(key);

    // locate subkey 1
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1ED63EE56FADC34D", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_primary_grip(key, &grip));
    assert_non_null(grip);
    assert_string_equal(grip, "66D6A0800A3FACDE0C0EB60B16B3669ED380FDFA");
    rnp_buffer_destroy(grip);
    grip = NULL;
    rnp_key_handle_destroy(key);

    // locate subkey 2
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1D7E8A5393C997A8", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_primary_grip(key, &grip));
    assert_non_null(grip);
    assert_string_equal(grip, "66D6A0800A3FACDE0C0EB60B16B3669ED380FDFA");
    rnp_buffer_destroy(grip);
    grip = NULL;
    rnp_key_handle_destroy(key);

    // locate subkey 3
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8A05B89FAD5ADED1", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_primary_grip(key, &grip));
    assert_non_null(grip);
    assert_string_equal(grip, "66D6A0800A3FACDE0C0EB60B16B3669ED380FDFA");
    rnp_buffer_destroy(grip);
    grip = NULL;
    rnp_key_handle_destroy(key);

    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_rnp_key_get_primary_fprint)
{
    rnp_ffi_t ffi = NULL;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    // load our keyrings
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));

    // locate primary key
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7BC6709B15C23A4A", &key));
    assert_non_null(key);

    // some edge cases
    char *fp = NULL;
    assert_rnp_failure(rnp_key_get_primary_fprint(NULL, NULL));
    assert_rnp_failure(rnp_key_get_primary_fprint(NULL, &fp));
    assert_rnp_failure(rnp_key_get_primary_fprint(key, NULL));
    assert_rnp_failure(rnp_key_get_primary_fprint(key, &fp));
    assert_null(fp);
    rnp_key_handle_destroy(key);

    // locate subkey 1
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1ED63EE56FADC34D", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_primary_fprint(key, &fp));
    assert_non_null(fp);
    assert_string_equal(fp, "E95A3CBF583AA80A2CCC53AA7BC6709B15C23A4A");
    rnp_buffer_destroy(fp);
    fp = NULL;
    rnp_key_handle_destroy(key);

    // locate subkey 2
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1D7E8A5393C997A8", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_primary_fprint(key, &fp));
    assert_non_null(fp);
    assert_string_equal(fp, "E95A3CBF583AA80A2CCC53AA7BC6709B15C23A4A");
    rnp_buffer_destroy(fp);
    fp = NULL;
    rnp_key_handle_destroy(key);

    // locate subkey 3
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8A05B89FAD5ADED1", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_primary_fprint(key, &fp));
    assert_non_null(fp);
    assert_string_equal(fp, "E95A3CBF583AA80A2CCC53AA7BC6709B15C23A4A");
    rnp_buffer_destroy(fp);
    fp = NULL;
    rnp_key_handle_destroy(key);

    // locate key 1 - subkey 0
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "54505A936A4A970E", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_primary_fprint(key, &fp));
    assert_non_null(fp);
    assert_string_equal(fp, "BE1C4AB951F4C2F6B604C7F82FCADF05FFA501BB");
    rnp_buffer_destroy(fp);
    fp = NULL;
    rnp_key_handle_destroy(key);

    // locate key 2 - subkey 1
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "326EF111425D14A5", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_primary_fprint(key, &fp));
    assert_non_null(fp);
    assert_string_equal(fp, "BE1C4AB951F4C2F6B604C7F82FCADF05FFA501BB");
    rnp_buffer_destroy(fp);
    fp = NULL;
    rnp_key_handle_destroy(key);

    // cleanup
    rnp_ffi_destroy(ffi);
}
