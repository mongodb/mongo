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

#include <rnp/rnp.h>
#include "rnp_tests.h"
#include "support.h"

TEST_F(rnp_tests, test_ffi_uid_properties)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(load_keys_gpg(ffi, "data/test_uid_validity/key-uids-pub.pgp"));

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "F6E741D1DF582D90", &key));
    assert_non_null(key);

    size_t uids = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 4);

    rnp_uid_handle_t uid = NULL;
    assert_rnp_failure(rnp_key_get_uid_handle_at(NULL, 0, &uid));
    assert_rnp_failure(rnp_key_get_uid_handle_at(key, 0, NULL));
    assert_rnp_failure(rnp_key_get_uid_handle_at(key, 100, &uid));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_non_null(uid);

    uint32_t uid_type = 0;
    assert_rnp_failure(rnp_uid_get_type(NULL, &uid_type));
    assert_rnp_failure(rnp_uid_get_type(uid, NULL));
    assert_rnp_success(rnp_uid_get_type(uid, &uid_type));
    assert_int_equal(uid_type, RNP_USER_ID);

    size_t size = 0;
    void * data = NULL;
    assert_rnp_failure(rnp_uid_get_data(NULL, &data, &size));
    assert_rnp_failure(rnp_uid_get_data(uid, NULL, &size));
    assert_rnp_failure(rnp_uid_get_data(uid, &data, NULL));
    assert_rnp_success(rnp_uid_get_data(uid, &data, &size));
    assert_int_equal(size, 12);
    assert_int_equal(memcmp(data, "userid-valid", size), 0);
    rnp_buffer_destroy(data);

    bool primary = false;
    assert_rnp_failure(rnp_uid_is_primary(NULL, &primary));
    assert_rnp_failure(rnp_uid_is_primary(uid, NULL));
    assert_rnp_success(rnp_uid_is_primary(uid, &primary));
    assert_true(primary);
    rnp_uid_handle_destroy(uid);

    assert_rnp_success(rnp_key_get_uid_handle_at(key, 1, &uid));
    assert_rnp_success(rnp_uid_get_data(uid, &data, &size));
    assert_int_equal(size, 14);
    assert_int_equal(memcmp(data, "userid-expired", size), 0);
    rnp_buffer_destroy(data);
    assert_rnp_success(rnp_uid_is_primary(uid, &primary));
    assert_false(primary);
    rnp_uid_handle_destroy(uid);

    assert_rnp_success(rnp_key_get_uid_handle_at(key, 2, &uid));
    assert_rnp_success(rnp_uid_get_data(uid, &data, &size));
    assert_int_equal(size, 14);
    assert_int_equal(memcmp(data, "userid-invalid", size), 0);
    rnp_buffer_destroy(data);
    assert_rnp_success(rnp_uid_is_primary(uid, &primary));
    assert_false(primary);
    rnp_uid_handle_destroy(uid);

    assert_rnp_success(rnp_key_get_uid_handle_at(key, 3, &uid));
    assert_rnp_success(rnp_uid_get_type(uid, &uid_type));
    assert_int_equal(uid_type, RNP_USER_ATTR);
    assert_rnp_success(rnp_uid_get_data(uid, &data, &size));
    assert_int_equal(size, 3038);
    rnp_buffer_destroy(data);
    assert_rnp_success(rnp_uid_is_primary(uid, &primary));
    assert_false(primary);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_uid_validity)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(load_keys_gpg(ffi, "data/test_uid_validity/key-uids-with-invalid.pgp"));

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "F6E741D1DF582D90", &key));
    assert_non_null(key);

    size_t uids = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 4);

    /* userid 0 : valid */
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    bool valid = false;
    assert_rnp_failure(rnp_uid_is_valid(NULL, &valid));
    assert_rnp_failure(rnp_uid_is_valid(uid, NULL));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    /* userid 1 : self-sig marks key as expired, but uid is still valid */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 1, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    /* userid 2 : invalid (malformed signature data) */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 2, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    rnp_uid_handle_destroy(uid);

    /* userid 3: valid userattr */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 3, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);

    /* Try to locate key via all uids */
    rnp_key_handle_t newkey = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-valid", &newkey));
    assert_non_null(newkey);
    rnp_key_handle_destroy(newkey);
    newkey = NULL;
    /* even if signature marks key as expired uid is still valid and usable */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-expired", &newkey));
    assert_non_null(newkey);
    rnp_key_handle_destroy(newkey);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-invalid", &newkey));
    assert_null(newkey);

    /* Now import key with valid signature for the userid 2 */
    assert_true(import_pub_keys(ffi, "data/test_uid_validity/key-uids-pub.pgp"));
    uids = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 4);

    /* userid 0 : valid */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    /* userid 1 : key is expired via self-cert, but uid is valid */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 1, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    /* userid 2 : valid */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 2, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    /* userid 3 : valid userattr */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 3, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);

    /* now we should be able to locate key via userid-invalid */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-invalid", &newkey));
    assert_non_null(newkey);
    rnp_key_handle_destroy(newkey);

    /* Now import key with revoked primary userid */
    assert_true(import_pub_keys(ffi, "data/test_uid_validity/key-uids-revoked-valid.pgp"));
    uids = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 4);

    /* userid 0 : invalid since it is revoked */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    bool primary = true;
    assert_rnp_success(rnp_uid_is_primary(uid, &primary));
    assert_false(primary);
    rnp_uid_handle_destroy(uid);

    /* Primary userid now should be userid-expired */
    char *uid_str = NULL;
    assert_rnp_success(rnp_key_get_primary_uid(key, &uid_str));
    assert_non_null(uid_str);
    assert_string_equal(uid_str, "userid-expired");
    rnp_buffer_destroy(uid_str);

    /* We should not be able to find key via userid-valid */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-valid", &newkey));
    assert_null(newkey);

    rnp_key_handle_destroy(key);

    /* Load expired key with single uid: still has primary uid as it has valid self-cert */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, "data/test_uid_validity/key-expired.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "4BE147BB22DF1E60", &key));
    assert_non_null(key);
    uid_str = NULL;
    assert_rnp_success(rnp_key_get_primary_uid(key, &uid_str));
    assert_string_equal(uid_str, "test1");
    rnp_buffer_destroy(uid_str);
    rnp_key_handle_destroy(key);

    /* UID with expired self-certification signature */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, "data/test_uid_validity/key-uid-expired-sig.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "expired_uid_sig", &key));
    assert_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "129195E05B0943CB", &key));
    assert_non_null(key);
    uid_str = NULL;
    assert_rnp_failure(rnp_key_get_primary_uid(key, &uid_str));
    assert_null(uid_str);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    rnp_uid_handle_destroy(uid);
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    /* key is valid since there is a subkey with valid binding signature */
    assert_true(valid);
    rnp_key_handle_destroy(key);

    /* UID with expired self-certification on primary uid signature */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, "data/test_uid_validity/key-uid-prim-expired-sig.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "expired_prim_uid_sig", &key));
    assert_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "non_prim_uid", &key));
    assert_non_null(key);
    uid_str = NULL;
    assert_rnp_success(rnp_key_get_primary_uid(key, &uid_str));
    assert_non_null(uid_str);
    assert_string_equal(uid_str, "non_prim_uid");
    rnp_buffer_destroy(uid_str);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    rnp_uid_handle_destroy(uid);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 1, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_remove_uid)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(load_keys_gpg(ffi, "data/test_uid_validity/key-uids-pub.pgp"));

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-valid", &key));
    size_t count = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 4);
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    rnp_uid_handle_t uid = NULL;
    /* delete last userattr */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 3, &uid));
    assert_rnp_failure(rnp_uid_remove(NULL, uid));
    assert_rnp_failure(rnp_uid_remove(key, NULL));
    assert_rnp_failure(rnp_uid_remove(sub, uid));
    assert_rnp_success(rnp_uid_remove(key, uid));
    assert_rnp_success(rnp_uid_handle_destroy(uid));
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 3);
    /* delete uid in the middle, userid-expired */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 1, &uid));
    char *uidstr = NULL;
    assert_rnp_success(rnp_key_get_uid_at(key, 1, &uidstr));
    assert_string_equal(uidstr, "userid-expired");
    rnp_buffer_destroy(uidstr);
    assert_rnp_success(rnp_uid_remove(key, uid));
    assert_rnp_success(rnp_uid_handle_destroy(uid));
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 2);
    /* delete first uid */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uidstr));
    assert_string_equal(uidstr, "userid-valid");
    rnp_buffer_destroy(uidstr);
    assert_rnp_success(rnp_uid_remove(key, uid));
    assert_rnp_success(rnp_uid_handle_destroy(uid));
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 1);
    /* delete last and only uid */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uidstr));
    assert_string_equal(uidstr, "userid-invalid");
    rnp_buffer_destroy(uidstr);
    assert_rnp_success(rnp_uid_remove(key, uid));
    assert_rnp_success(rnp_uid_handle_destroy(uid));
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 0);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);
    /* now let's reload pubring to make sure that they are removed */
    reload_pubring(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-valid", &key));
    assert_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "f6e741d1df582d90", &key));
    count = 255;
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 0);
    rnp_key_handle_destroy(key);

    /* delete userids of the secret key and reload */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(load_keys_gpg(ffi,
                              "data/test_uid_validity/key-uids-pub.pgp",
                              "data/test_uid_validity/key-uids-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-valid", &key));
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 4);
    bool secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    /* remove userid-expired */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 1, &uid));
    assert_rnp_success(rnp_key_get_uid_at(key, 1, &uidstr));
    assert_string_equal(uidstr, "userid-expired");
    rnp_buffer_destroy(uidstr);
    assert_rnp_success(rnp_uid_remove(key, uid));
    assert_rnp_success(rnp_uid_handle_destroy(uid));
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 3);
    /* remove userid-invalid */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 1, &uid));
    assert_rnp_success(rnp_key_get_uid_at(key, 1, &uidstr));
    assert_string_equal(uidstr, "userid-invalid");
    rnp_buffer_destroy(uidstr);
    assert_rnp_success(rnp_uid_remove(key, uid));
    assert_rnp_success(rnp_uid_handle_destroy(uid));
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 2);
    rnp_key_handle_destroy(key);
    /* reload */
    reload_keyrings(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "userid-valid", &key));
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 2);
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}
