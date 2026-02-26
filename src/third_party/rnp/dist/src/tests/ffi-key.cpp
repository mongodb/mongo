/*
 * Copyright (c) 2022-2023 [Ribose Inc](https://www.ribose.com).
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
#include <librepgp/stream-ctx.h>
#include "key.hpp"
#include "ffi-priv-types.h"
#include "str-utils.h"
#ifndef RNP_USE_STD_REGEX
#include <regex.h>
#else
#include <regex>
#endif

static void
check_key_properties(rnp_key_handle_t key,
                     bool             primary_expected,
                     bool             have_public_expected,
                     bool             have_secret_expected)
{
    bool isprimary = !primary_expected;
    assert_rnp_success(rnp_key_is_primary(key, &isprimary));
    assert_true(isprimary == primary_expected);
    bool issub = primary_expected;
    assert_rnp_success(rnp_key_is_sub(key, &issub));
    assert_true(issub == !primary_expected);
    bool have_public = !have_public_expected;
    assert_rnp_success(rnp_key_have_public(key, &have_public));
    assert_true(have_public == have_public_expected);
    bool have_secret = !have_secret_expected;
    assert_rnp_success(rnp_key_have_secret(key, &have_secret));
    assert_true(have_secret == have_secret_expected);
}

TEST_F(rnp_tests, test_ffi_keygen_json_pair)
{
    rnp_ffi_t ffi = NULL;
    char *    results = NULL;
    size_t    count = 0;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "abc"));

    // load our JSON
    auto json = file_to_str("data/test_ffi_json/generate-pair.json");

    // generate the keys
    assert_rnp_success(rnp_generate_key_json(ffi, json.c_str(), &results));
    assert_non_null(results);

    // parse the results JSON
    json_object *parsed_results = json_tokener_parse(results);
    assert_non_null(parsed_results);
    rnp_buffer_destroy(results);
    results = NULL;
    // get a handle for the primary
    rnp_key_handle_t primary = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "primary", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        const char *grip = json_object_get_string(jsogrip);
        assert_non_null(grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", grip, &primary));
        assert_non_null(primary);
    }
    // get a handle for the sub
    rnp_key_handle_t sub = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "sub", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        const char *grip = json_object_get_string(jsogrip);
        assert_non_null(grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", grip, &sub));
        assert_non_null(sub);
    }
    // cleanup
    json_object_put(parsed_results);

    // check the key counts
    assert_rnp_failure(rnp_get_public_key_count(NULL, &count));
    assert_rnp_failure(rnp_get_public_key_count(ffi, NULL));
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(2, count);
    assert_rnp_failure(rnp_get_secret_key_count(NULL, &count));
    assert_rnp_failure(rnp_get_secret_key_count(ffi, NULL));
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(2, count);

    // check some key properties
    check_key_properties(primary, true, true, true);
    check_key_properties(sub, false, true, true);

    // check sub bit length
    uint32_t length = 0;
    assert_rnp_success(rnp_key_get_bits(sub, &length));
    assert_int_equal(1024, length);

    // cleanup
    rnp_key_handle_destroy(primary);
    rnp_key_handle_destroy(sub);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_keygen_json_pair_dsa_elg)
{
    rnp_ffi_t ffi = NULL;
    char *    results = NULL;
    size_t    count = 0;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "abc"));

    // load our JSON
    auto json = file_to_str("data/test_ffi_json/generate-pair-dsa-elg.json");

    // generate the keys
    assert_rnp_success(rnp_generate_key_json(ffi, json.c_str(), &results));
    assert_non_null(results);

    // parse the results JSON
    json_object *parsed_results = json_tokener_parse(results);
    assert_non_null(parsed_results);
    rnp_buffer_destroy(results);
    results = NULL;
    // get a handle for the primary
    rnp_key_handle_t primary = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "primary", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        const char *grip = json_object_get_string(jsogrip);
        assert_non_null(grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", grip, &primary));
        assert_non_null(primary);
    }
    // get a handle for the sub
    rnp_key_handle_t sub = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "sub", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        const char *grip = json_object_get_string(jsogrip);
        assert_non_null(grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", grip, &sub));
        assert_non_null(sub);
    }
    // cleanup
    json_object_put(parsed_results);

    // check the key counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(2, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(2, count);

    // check some key properties
    check_key_properties(primary, true, true, true);
    check_key_properties(sub, false, true, true);

    // check bit lengths
    uint32_t length = 0;
    assert_rnp_success(rnp_key_get_bits(primary, &length));
    assert_int_equal(length, 1024);
    assert_rnp_success(rnp_key_get_bits(sub, &length));
    assert_int_equal(length, 1536);

    // cleanup
    rnp_key_handle_destroy(primary);
    rnp_key_handle_destroy(sub);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_keygen_json_primary)
{
    rnp_ffi_t ffi = NULL;
    char *    results = NULL;
    size_t    count = 0;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, unused_getpasscb, NULL));

    // load our JSON
    auto json = file_to_str("data/test_ffi_json/generate-primary.json");

    // generate the keys
    assert_rnp_success(rnp_generate_key_json(ffi, json.c_str(), &results));
    assert_non_null(results);

    // parse the results JSON
    json_object *parsed_results = json_tokener_parse(results);
    assert_non_null(parsed_results);
    rnp_buffer_destroy(results);
    results = NULL;
    // get a handle for the primary
    rnp_key_handle_t primary = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "primary", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        const char *grip = json_object_get_string(jsogrip);
        assert_non_null(grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", grip, &primary));
        assert_non_null(primary);
    }
    // cleanup
    json_object_put(parsed_results);
    parsed_results = NULL;

    // check the key counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(1, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(1, count);

    // check some key properties
    check_key_properties(primary, true, true, true);

    // cleanup
    rnp_key_handle_destroy(primary);
    rnp_ffi_destroy(ffi);
}

/* This test generates a primary key, and then a subkey (separately).
 */
TEST_F(rnp_tests, test_ffi_keygen_json_sub)
{
    char *    results = NULL;
    size_t    count = 0;
    rnp_ffi_t ffi = NULL;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, unused_getpasscb, NULL));

    // generate our primary key
    auto json = file_to_str("data/test_ffi_json/generate-primary.json");
    assert_rnp_success(rnp_generate_key_json(ffi, json.c_str(), &results));
    // check key counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(1, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(1, count);

    // parse the results JSON
    json_object *parsed_results = json_tokener_parse(results);
    assert_non_null(parsed_results);
    rnp_buffer_destroy(results);
    results = NULL;
    // get a handle+grip for the primary
    rnp_key_handle_t primary = NULL;
    char *           primary_grip = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "primary", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        primary_grip = strdup(json_object_get_string(jsogrip));
        assert_non_null(primary_grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", primary_grip, &primary));
        assert_non_null(primary);
    }
    // cleanup
    json_object_put(parsed_results);
    parsed_results = NULL;

    // load our JSON template
    json = file_to_str("data/test_ffi_json/generate-sub.json");
    // modify our JSON
    {
        // parse
        json_object *jso = json_tokener_parse(json.c_str());
        assert_non_null(jso);
        // find the relevant fields
        json_object *jsosub = NULL;
        json_object *jsoprimary = NULL;
        assert_true(json_object_object_get_ex(jso, "sub", &jsosub));
        assert_non_null(jsosub);
        assert_true(json_object_object_get_ex(jsosub, "primary", &jsoprimary));
        assert_non_null(jsoprimary);
        // replace the placeholder grip with the correct one
        json_object_object_del(jsoprimary, "grip");
        json_object_object_add(jsoprimary, "grip", json_object_new_string(primary_grip));
        assert_int_equal(1, json_object_object_length(jsoprimary));
        json = json_object_to_json_string_ext(jso, JSON_C_TO_STRING_PRETTY);
        assert_false(json.empty());
        json_object_put(jso);
    }
    // cleanup
    rnp_buffer_destroy(primary_grip);
    primary_grip = NULL;

    // generate the subkey
    assert_rnp_success(rnp_generate_key_json(ffi, json.c_str(), &results));
    assert_non_null(results);

    // parse the results JSON
    parsed_results = json_tokener_parse(results);
    assert_non_null(parsed_results);
    rnp_buffer_destroy(results);
    results = NULL;
    // get a handle for the sub
    rnp_key_handle_t sub = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "sub", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        const char *grip = json_object_get_string(jsogrip);
        assert_non_null(grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", grip, &sub));
        assert_non_null(sub);
    }
    // cleanup
    json_object_put(parsed_results);
    parsed_results = NULL;

    // check the key counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(2, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(2, count);

    // check some key properties
    check_key_properties(primary, true, true, true);
    check_key_properties(sub, false, true, true);

    // check sub bit length
    uint32_t length = 0;
    assert_rnp_success(rnp_key_get_bits(sub, &length));
    assert_int_equal(length, 1024);

    // cleanup
    rnp_key_handle_destroy(primary);
    rnp_key_handle_destroy(sub);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_keygen_json_edge_cases)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* Attempt to generate with invalid parameters */
    std::string json = "";
    char *      results = NULL;
    assert_rnp_failure(rnp_generate_key_json(NULL, json.c_str(), &results));
    assert_rnp_failure(rnp_generate_key_json(ffi, NULL, &results));
    assert_rnp_failure(rnp_generate_key_json(ffi, "{ something, wrong }", &results));
    assert_rnp_failure(rnp_generate_key_json(ffi, "{ }", &results));
    assert_rnp_failure(
      rnp_generate_key_json(ffi, "{ \"primary\": { }, \"wrong\": {} }", &results));
    assert_rnp_failure(
      rnp_generate_key_json(ffi, "{ \"primary\": { }, \"PRIMARY\": { } }", &results));
    /* Json-C puts stuff under the same key into the single object */
    assert_rnp_success(
      rnp_generate_key_json(ffi, "{ \"primary\": { }, \"primary\": { } }", &results));
    rnp_buffer_destroy(results);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    /* Generate key with an empty description */
    assert_rnp_success(rnp_generate_key_json(ffi, "{ \"priMary\": {} }", &results));
    assert_non_null(results);
    rnp_buffer_destroy(results);
    size_t count = 0;
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    /* Generate with wrong preferences */
    json = file_to_str("data/test_ffi_json/generate-eddsa-wrong-prefs.json");
    assert_rnp_failure(rnp_generate_key_json(ffi, json.c_str(), &results));
    /* Generate with wrong PK algorithm */
    json = file_to_str("data/test_ffi_json/generate-bad-pk-alg.json");
    results = NULL;
    assert_rnp_failure(rnp_generate_key_json(ffi, json.c_str(), &results));
    assert_null(results);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 0);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_generate_misc)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_failure(rnp_ffi_set_key_provider(NULL, unused_getkeycb, NULL));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));

    /* make sure we do not leak key handle and do not access NULL */
    assert_rnp_success(rnp_generate_key_rsa(ffi, 1024, 1024, "rsa", NULL, NULL));

    /* make sure we do not leak password on failed key generation */
    rnp_key_handle_t key = NULL;
    assert_rnp_failure(rnp_generate_key_rsa(ffi, 768, 2048, "rsa_768", "password", &key));
    assert_rnp_failure(rnp_generate_key_rsa(ffi, 1024, 768, "rsa_768", "password", &key));

    /* make sure we behave correctly and do not leak data on wrong parameters to _generate_ex
     * function */
    assert_rnp_failure(rnp_generate_key_ex(
      ffi, "RSA", "RSA", 1024, 1024, "Curve", NULL, "userid", "password", &key));
    assert_rnp_failure(rnp_generate_key_ex(
      ffi, "RSA", "RSA", 1024, 1024, "Curve", NULL, NULL, "password", &key));
    assert_rnp_failure(rnp_generate_key_ex(
      ffi, "RSA", "RSA", 1024, 768, NULL, "Curve", NULL, "password", &key));
    assert_rnp_failure(rnp_generate_key_ex(
      ffi, "ECDSA", "ECDH", 1024, 0, "Unknown", "Curve", NULL, NULL, &key));
    assert_rnp_failure(rnp_generate_key_ex(
      ffi, "ECDSA", "ECDH", 0, 1024, "Unknown", "Curve", NULL, "password", &key));

    /* generate RSA-RSA key without password */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "abc"));
    assert_rnp_success(rnp_generate_key_rsa(ffi, 1024, 1024, "rsa_1024", NULL, &key));
    assert_non_null(key);
    bool locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_false(locked);
    /* check key and subkey flags */
    bool flag = false;
    assert_rnp_success(rnp_key_allows_usage(key, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "certify", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "authenticate", &flag));
    assert_false(flag);
    uint32_t expiration = 0;
    assert_rnp_success(rnp_key_get_expiration(key, &expiration));
    assert_int_equal(expiration, 2 * 365 * 24 * 60 * 60);
    uint32_t creation = 0;
    assert_rnp_success(rnp_key_get_creation(key, &creation));
    uint32_t till = 0;
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, creation + expiration);
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    expiration = 0;
    assert_rnp_success(rnp_key_get_expiration(subkey, &expiration));
    assert_int_equal(expiration, 2 * 365 * 24 * 60 * 60);
    creation = 0;
    assert_rnp_success(rnp_key_get_creation(key, &creation));
    till = 0;
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, creation + expiration);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    /* generate encrypted RSA-RSA key */
    assert_rnp_success(rnp_generate_key_rsa(ffi, 1024, 1024, "rsa_1024", "123", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    /* make sure it can be unlocked with correct password */
    assert_rnp_success(rnp_key_unlock(key, "123"));
    /* do the same for subkey */
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_key_is_locked(subkey, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_unlock(subkey, "123"));
    /* cleanup */
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    /* generate encrypted RSA key (primary only) */
    key = NULL;
    assert_rnp_success(
      rnp_generate_key_ex(ffi, "RSA", NULL, 1024, 0, NULL, NULL, "rsa_1024", "123", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    bool prot = false;
    assert_rnp_success(rnp_key_is_protected(key, &prot));
    assert_true(prot);
    /* cleanup */
    rnp_key_handle_destroy(key);
    /* generate key with signing subkey using rnp_generate_key_ex() */
    key = NULL;
    subkey = NULL;
    flag = false;
    assert_rnp_success(rnp_generate_key_ex(
      ffi, "ECDSA", "ECDSA", 0, 0, "secp256k1", "NIST P-256", "ex_sign", NULL, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_false(flag);
    rnp_key_handle_destroy(subkey);
    rnp_key_handle_destroy(key);

    /* generate key with signing subkey */
    rnp_op_generate_t op = NULL;
    assert_rnp_success(rnp_op_generate_create(&op, ffi, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_curve(op, "secp256k1"));
    assert_rnp_success(rnp_op_generate_set_userid(op, "ecdsa_ecdsa"));
    assert_rnp_success(rnp_op_generate_add_usage(op, "sign"));
    assert_rnp_success(rnp_op_generate_add_usage(op, "certify"));
    assert_rnp_success(rnp_op_generate_set_expiration(op, 0));
    assert_rnp_success(rnp_op_generate_execute(op));
    rnp_key_handle_t primary = NULL;
    assert_rnp_success(rnp_op_generate_get_key(op, &primary));
    rnp_op_generate_destroy(op);
    char *keyid = NULL;
    assert_rnp_success(rnp_key_get_keyid(primary, &keyid));

    rnp_op_generate_t subop = NULL;
    assert_rnp_failure(rnp_op_generate_subkey_create(NULL, ffi, primary, "ECDSA"));
    assert_rnp_failure(rnp_op_generate_subkey_create(&subop, NULL, primary, "ECDSA"));
    assert_rnp_failure(rnp_op_generate_subkey_create(&subop, ffi, NULL, "ECDSA"));
    assert_rnp_failure(rnp_op_generate_subkey_create(&subop, ffi, primary, NULL));
    assert_rnp_success(rnp_op_generate_subkey_create(&subop, ffi, primary, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_curve(subop, "NIST P-256"));
    assert_rnp_success(rnp_op_generate_add_usage(subop, "sign"));
    assert_rnp_success(rnp_op_generate_add_usage(subop, "certify"));
    assert_rnp_success(rnp_op_generate_set_expiration(subop, 0));
    assert_rnp_success(rnp_op_generate_execute(subop));
    assert_rnp_success(rnp_op_generate_get_key(subop, &subkey));
    rnp_op_generate_destroy(subop);
    char *subid = NULL;
    assert_rnp_success(rnp_key_get_keyid(subkey, &subid));

    rnp_output_t output = NULL;
    rnp_output_to_memory(&output, 0);
    assert_rnp_failure(rnp_key_export(NULL, output, RNP_KEY_EXPORT_PUBLIC));
    assert_rnp_failure(rnp_key_export(primary, NULL, RNP_KEY_EXPORT_PUBLIC));
    assert_rnp_failure(rnp_key_export(primary, output, 0));
    assert_rnp_failure(
      rnp_key_export(primary, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SECRET));
    assert_rnp_failure(rnp_key_export(primary, output, 0x77));

    assert_rnp_success(
      rnp_key_export(primary,
                     output,
                     RNP_KEY_EXPORT_ARMORED | RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS));
    rnp_key_handle_destroy(primary);
    rnp_key_handle_destroy(subkey);
    uint8_t *buf = NULL;
    size_t   len = 0;
    rnp_output_memory_get_buf(output, &buf, &len, false);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, buf, len));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", keyid, &primary));
    assert_non_null(primary);
    assert_true(primary->pub->valid());
    bool valid = false;
    assert_rnp_failure(rnp_key_is_valid(primary, NULL));
    assert_rnp_failure(rnp_key_is_valid(NULL, &valid));
    assert_rnp_success(rnp_key_is_valid(primary, &valid));
    assert_true(valid);
    till = 0;
    assert_rnp_failure(rnp_key_valid_till(primary, NULL));
    assert_rnp_failure(rnp_key_valid_till(NULL, &till));
    assert_rnp_success(rnp_key_valid_till(primary, &till));
    assert_int_equal(till, 0xffffffff);
    uint64_t till64 = 0;
    assert_rnp_failure(rnp_key_valid_till64(primary, NULL));
    assert_rnp_failure(rnp_key_valid_till64(NULL, &till64));
    assert_rnp_success(rnp_key_valid_till64(primary, &till64));
    assert_int_equal(till64, UINT64_MAX);
    rnp_key_handle_destroy(primary);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", subid, &subkey));
    assert_non_null(subkey);
    assert_true(subkey->pub->valid());
    valid = false;
    assert_rnp_success(rnp_key_is_valid(subkey, &valid));
    assert_true(valid);
    till = 0;
    assert_rnp_success(rnp_key_valid_till(subkey, &till));
    assert_int_equal(till, 0xffffffff);
    assert_rnp_success(rnp_key_valid_till64(subkey, &till64));
    assert_int_equal(till64, UINT64_MAX);
    rnp_key_handle_destroy(subkey);
    rnp_buffer_destroy(keyid);
    rnp_buffer_destroy(subid);
    rnp_output_destroy(output);

    /* cleanup */
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_sec_key_offline_operations)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* generate subkey for offline secret key */
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-1-subs.pgp"));
    rnp_key_handle_t primary = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &primary));
    rnp_op_generate_t subop = NULL;
    assert_rnp_failure(rnp_op_generate_subkey_create(&subop, ffi, primary, "ECDSA"));
    /* unlock/unprotect offline secret key */
    assert_rnp_failure(rnp_key_unlock(primary, "password"));
    assert_rnp_failure(rnp_key_unprotect(primary, "password"));
    /* add userid */
    assert_int_equal(rnp_key_add_uid(primary, "new_uid", "SHA256", 2147317200, 0x00, false),
                     RNP_ERROR_NO_SUITABLE_KEY);
    rnp_key_handle_destroy(primary);
    /* generate subkey for offline secret key on card */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-2-card.pgp"));
    primary = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &primary));
    subop = NULL;
    assert_rnp_failure(rnp_op_generate_subkey_create(&subop, ffi, primary, "ECDSA"));
    rnp_key_handle_destroy(primary);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_generate_rsa)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    /* make sure we fail to generate too small and too large keys/subkeys */
    rnp_key_handle_t key = NULL;
    assert_rnp_failure(rnp_generate_key_rsa(ffi, 768, 2048, "rsa_768", NULL, &key));
    assert_rnp_failure(rnp_generate_key_rsa(ffi, 1024, 768, "rsa_768", NULL, &key));
    assert_rnp_failure(rnp_generate_key_rsa(ffi, 20480, 1024, "rsa_20480", NULL, &key));
    assert_rnp_failure(rnp_generate_key_rsa(ffi, 1024, 20480, "rsa_20480", NULL, &key));
    /* generate RSA-RSA key */
    assert_rnp_success(rnp_generate_key_rsa(ffi, 1024, 2048, "rsa_1024", NULL, &key));
    assert_non_null(key);
    /* check properties of the generated key */
    bool boolres = false;
    assert_rnp_success(rnp_key_is_primary(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_public(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(key, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(key, &boolres));
    assert_false(boolres);
    /* algorithm */
    char *alg = NULL;
    assert_rnp_success(rnp_key_get_alg(key, &alg));
    assert_int_equal(strcasecmp(alg, "RSA"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    uint32_t bits = 0;
    assert_rnp_failure(rnp_key_get_bits(key, NULL));
    assert_rnp_success(rnp_key_get_bits(key, &bits));
    assert_int_equal(bits, 1024);
    assert_rnp_failure(rnp_key_get_dsa_qbits(key, &bits));
    /* key flags */
    bool flag = false;
    assert_rnp_success(rnp_key_allows_usage(key, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "certify", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "authenticate", &flag));
    assert_false(flag);
    /* curve - must fail */
    char *curve = NULL;
    assert_rnp_failure(rnp_key_get_curve(key, NULL));
    assert_rnp_failure(rnp_key_get_curve(key, &curve));
    assert_null(curve);
    /* user ids */
    size_t uids = 0;
    char * uid = NULL;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 1);
    assert_rnp_failure(rnp_key_get_uid_at(key, 1, &uid));
    assert_null(uid);
    assert_rnp_failure(rnp_key_get_uid_at(NULL, 0, &uid));
    assert_rnp_failure(rnp_key_get_uid_at(key, 0, NULL));
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uid));
    assert_string_equal(uid, "rsa_1024");
    rnp_buffer_destroy(uid);
    /* subkey */
    size_t subkeys = 0;
    assert_rnp_failure(rnp_key_get_subkey_count(key, NULL));
    assert_rnp_success(rnp_key_get_subkey_count(key, &subkeys));
    assert_int_equal(subkeys, 1);
    rnp_key_handle_t subkey = NULL;
    assert_rnp_failure(rnp_key_get_subkey_at(key, 1, &subkey));
    assert_rnp_failure(rnp_key_get_subkey_at(key, 0, NULL));
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    /* check properties of the generated subkey */
    assert_rnp_success(rnp_key_is_primary(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_have_public(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(subkey, &boolres));
    assert_false(boolres);
    /* algorithm */
    assert_rnp_success(rnp_key_get_alg(subkey, &alg));
    assert_int_equal(strcasecmp(alg, "RSA"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    assert_rnp_success(rnp_key_get_bits(subkey, &bits));
    assert_int_equal(bits, 2048);
    /* subkey flags */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    /* cleanup */
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* generate RSA key without the subkey */
    assert_rnp_success(rnp_generate_key_rsa(ffi, 1024, 0, "rsa_1024", NULL, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_subkey_count(key, &subkeys));
    assert_int_equal(subkeys, 0);
    /* cleanup */
    assert_rnp_success(rnp_key_handle_destroy(key));
    /* generate RSA keypair with default sizes */
    assert_rnp_success(rnp_generate_key_ex(
      ffi, RNP_ALGNAME_RSA, RNP_ALGNAME_RSA, 0, 0, NULL, NULL, "rsa_default", NULL, &key));
    assert_rnp_success(rnp_key_get_bits(key, &bits));
    assert_int_equal(bits, 3072);
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    assert_rnp_success(rnp_key_get_bits(subkey, &bits));
    assert_int_equal(bits, 3072);
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_generate_dsa)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    /* try to generate keys with invalid sizes */
    rnp_key_handle_t key = NULL;
    assert_rnp_failure(rnp_generate_key_dsa_eg(ffi, 768, 2048, "dsa_768", NULL, &key));
    assert_rnp_failure(rnp_generate_key_dsa_eg(ffi, 1024, 768, "dsa_768", NULL, &key));
    assert_rnp_failure(rnp_generate_key_dsa_eg(ffi, 4096, 1024, "dsa_20480", NULL, &key));
    assert_rnp_failure(rnp_generate_key_dsa_eg(ffi, 1024, 20480, "dsa_20480", NULL, &key));
    /* generate DSA-ElGamal keypair */
    assert_rnp_success(rnp_generate_key_dsa_eg(ffi, 1024, 1024, "dsa_1024", NULL, &key));
    assert_non_null(key);
    /* check properties of the generated key */
    bool boolres = false;
    assert_rnp_success(rnp_key_is_primary(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_public(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(key, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(key, &boolres));
    assert_false(boolres);
    /* algorithm */
    char *alg = NULL;
    assert_rnp_success(rnp_key_get_alg(key, &alg));
    assert_int_equal(strcasecmp(alg, "DSA"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    uint32_t bits = 0;
    assert_rnp_success(rnp_key_get_bits(key, &bits));
    assert_int_equal(bits, 1024);
    assert_rnp_success(rnp_key_get_dsa_qbits(key, &bits));
    assert_int_equal(bits, 160);
    /* key flags */
    bool flag = false;
    assert_rnp_success(rnp_key_allows_usage(key, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "certify", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "authenticate", &flag));
    assert_false(flag);
    /* user ids */
    size_t uids = 0;
    char * uid = NULL;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 1);
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uid));
    assert_string_equal(uid, "dsa_1024");
    rnp_buffer_destroy(uid);
    /* subkey */
    size_t subkeys = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &subkeys));
    assert_int_equal(subkeys, 1);
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    /* check properties of the generated subkey */
    assert_rnp_success(rnp_key_is_primary(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_have_public(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(subkey, &boolres));
    assert_false(boolres);
    /* algorithm */
    assert_rnp_success(rnp_key_get_alg(subkey, &alg));
    assert_int_equal(strcasecmp(alg, "ELGAMAL"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    assert_rnp_success(rnp_key_get_bits(subkey, &bits));
    assert_int_equal(bits, 1024);
    /* subkey flags */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    /* cleanup */
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* generate DSA key without the subkey */
    assert_rnp_success(rnp_generate_key_dsa_eg(ffi, 1024, 0, "dsa_1024", NULL, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_get_subkey_count(key, &subkeys));
    assert_int_equal(subkeys, 0);
    /* make sure we fail to generate 4096-bit DSA key */
    assert_rnp_failure(rnp_generate_key_dsa_eg(ffi, 4096, 0, "dsa_4096", NULL, &key));
    /* cleanup */
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_generate_ecdsa)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    /* try to generate key with invalid curve */
    rnp_key_handle_t key = NULL;
    assert_rnp_failure(rnp_generate_key_ec(ffi, "curve_wrong", "wrong", NULL, &key));
    assert_null(key);
    /* generate secp256k1 key */
    assert_rnp_success(rnp_generate_key_ec(ffi, "secp256k1", "ec_256k1", NULL, &key));
    assert_non_null(key);
    /* check properties of the generated key */
    bool boolres = false;
    assert_rnp_success(rnp_key_is_primary(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_public(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(key, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(key, &boolres));
    assert_false(boolres);
    /* algorithm */
    char *alg = NULL;
    assert_rnp_success(rnp_key_get_alg(key, &alg));
    assert_int_equal(strcasecmp(alg, "ECDSA"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    uint32_t bits = 0;
    assert_rnp_success(rnp_key_get_bits(key, &bits));
    assert_int_equal(bits, 256);
    assert_rnp_failure(rnp_key_get_dsa_qbits(key, &bits));
    /* curve */
    char *curve = NULL;
    assert_rnp_failure(rnp_key_get_curve(key, NULL));
    assert_rnp_success(rnp_key_get_curve(key, &curve));
    assert_int_equal(strcasecmp(curve, "secp256k1"), 0);
    rnp_buffer_destroy(curve);
    /* key flags */
    bool flag = false;
    assert_rnp_success(rnp_key_allows_usage(key, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "certify", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "authenticate", &flag));
    assert_false(flag);
    /* user ids */
    size_t uids = 0;
    char * uid = NULL;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 1);
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uid));
    assert_string_equal(uid, "ec_256k1");
    rnp_buffer_destroy(uid);
    /* subkey */
    size_t subkeys = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &subkeys));
    assert_int_equal(subkeys, 1);
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    /* check properties of the generated subkey */
    assert_rnp_success(rnp_key_is_primary(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_have_public(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(subkey, &boolres));
    assert_false(boolres);
    /* algorithm */
    assert_rnp_success(rnp_key_get_alg(subkey, &alg));
    assert_int_equal(strcasecmp(alg, "ECDH"), 0);
    rnp_buffer_destroy(alg);
    /* bits */
    assert_rnp_success(rnp_key_get_bits(subkey, &bits));
    assert_int_equal(bits, 256);
    /* curve */
    curve = NULL;
    assert_rnp_success(rnp_key_get_curve(subkey, &curve));
    assert_int_equal(strcasecmp(curve, "secp256k1"), 0);
    rnp_buffer_destroy(curve);
    /* subkey flags */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);

    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_generate_eddsa)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    /* generate key with subkey */
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_generate_key_25519(ffi, "eddsa_25519", NULL, &key));
    assert_non_null(key);
    /* check properties of the generated key */
    bool boolres = false;
    assert_rnp_success(rnp_key_is_primary(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_public(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(key, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(key, &boolres));
    assert_false(boolres);
    /* algorithm */
    char *alg = NULL;
    assert_rnp_success(rnp_key_get_alg(key, &alg));
    assert_int_equal(strcasecmp(alg, "EDDSA"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    uint32_t bits = 0;
    assert_rnp_success(rnp_key_get_bits(key, &bits));
    assert_int_equal(bits, 255);
    /* curve */
    char *curve = NULL;
    assert_rnp_success(rnp_key_get_curve(key, &curve));
    assert_int_equal(strcasecmp(curve, "ed25519"), 0);
    rnp_buffer_destroy(curve);
    /* key flags */
    bool flag = false;
    assert_rnp_success(rnp_key_allows_usage(key, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "certify", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "authenticate", &flag));
    assert_false(flag);
    /* user ids */
    size_t uids = 0;
    char * uid = NULL;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 1);
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uid));
    assert_string_equal(uid, "eddsa_25519");
    rnp_buffer_destroy(uid);
    /* subkey */
    size_t subkeys = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &subkeys));
    assert_int_equal(subkeys, 1);
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    /* check properties of the generated subkey */
    assert_rnp_success(rnp_key_is_primary(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_have_public(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(subkey, &boolres));
    assert_false(boolres);
    /* algorithm */
    assert_rnp_success(rnp_key_get_alg(subkey, &alg));
    assert_int_equal(strcasecmp(alg, "ECDH"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    assert_rnp_success(rnp_key_get_bits(subkey, &bits));
    assert_int_equal(bits, 255);
    /* curve */
    curve = NULL;
    assert_rnp_success(rnp_key_get_curve(subkey, &curve));
    assert_int_equal(strcasecmp(curve, "Curve25519"), 0);
    rnp_buffer_destroy(curve);
    /* subkey flags */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);

    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_generate_sm2)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));

    /* generate sm2 key */
    rnp_key_handle_t key = NULL;
    if (!sm2_enabled()) {
        assert_rnp_failure(rnp_generate_key_sm2(ffi, "sm2", NULL, &key));
        assert_rnp_success(rnp_ffi_destroy(ffi));
        return;
    }
    assert_rnp_success(rnp_generate_key_sm2(ffi, "sm2", NULL, &key));
    assert_non_null(key);
    /* check properties of the generated key */
    bool boolres = false;
    assert_rnp_success(rnp_key_is_primary(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_public(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(key, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(key, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(key, &boolres));
    assert_false(boolres);
    /* algorithm */
    char *alg = NULL;
    assert_rnp_success(rnp_key_get_alg(key, &alg));
    assert_int_equal(strcasecmp(alg, "SM2"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    uint32_t bits = 0;
    assert_rnp_success(rnp_key_get_bits(key, &bits));
    assert_int_equal(bits, 256);
    /* curve */
    char *curve = NULL;
    assert_rnp_success(rnp_key_get_curve(key, &curve));
    assert_int_equal(strcasecmp(curve, "SM2 P-256"), 0);
    rnp_buffer_destroy(curve);
    /* key flags */
    bool flag = false;
    assert_rnp_success(rnp_key_allows_usage(key, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "certify", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "authenticate", &flag));
    assert_false(flag);
    /* user ids */
    size_t uids = 0;
    char * uid = NULL;
    assert_rnp_success(rnp_key_get_uid_count(key, &uids));
    assert_int_equal(uids, 1);
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uid));
    assert_string_equal(uid, "sm2");
    rnp_buffer_destroy(uid);
    /* subkey */
    size_t subkeys = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &subkeys));
    assert_int_equal(subkeys, 1);
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    /* check properties of the generated subkey */
    assert_rnp_success(rnp_key_is_primary(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_have_public(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_have_secret(subkey, &boolres));
    assert_true(boolres);
    assert_rnp_success(rnp_key_is_protected(subkey, &boolres));
    assert_false(boolres);
    assert_rnp_success(rnp_key_is_locked(subkey, &boolres));
    assert_false(boolres);
    /* algorithm */
    assert_rnp_success(rnp_key_get_alg(subkey, &alg));
    assert_int_equal(strcasecmp(alg, "SM2"), 0);
    rnp_buffer_destroy(alg);
    /* key bits */
    assert_rnp_success(rnp_key_get_bits(subkey, &bits));
    assert_int_equal(bits, 256);
    /* curve */
    curve = NULL;
    assert_rnp_success(rnp_key_get_curve(subkey, &curve));
    assert_int_equal(strcasecmp(curve, "SM2 P-256"), 0);
    rnp_buffer_destroy(curve);
    /* subkey flags */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);

    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_generate_ex)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "123"));

    /* Generate RSA key with misc options set */
    rnp_op_generate_t keygen = NULL;
    assert_rnp_success(rnp_op_generate_create(&keygen, ffi, "RSA"));
    assert_rnp_failure(rnp_op_generate_set_bits(NULL, 1024));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_failure(rnp_op_generate_set_dsa_qbits(keygen, 256));
    /* key usage */
    assert_rnp_success(rnp_op_generate_clear_usage(keygen));
    assert_rnp_failure(rnp_op_generate_add_usage(keygen, "usage"));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "sign"));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "encrypt"));
    /* preferred ciphers */
    assert_rnp_success(rnp_op_generate_clear_pref_ciphers(keygen));
    assert_rnp_failure(rnp_op_generate_add_pref_cipher(keygen, "unknown"));
    assert_true(!rnp_op_generate_add_pref_cipher(keygen, "BLOWFISH") == blowfish_enabled());
    assert_rnp_success(rnp_op_generate_clear_pref_ciphers(keygen));
    assert_rnp_success(rnp_op_generate_add_pref_cipher(keygen, "CAMELLIA256"));
    assert_rnp_success(rnp_op_generate_add_pref_cipher(keygen, "AES256"));
    /* preferred compression algorithms */
    assert_rnp_success(rnp_op_generate_clear_pref_compression(keygen));
    assert_rnp_failure(rnp_op_generate_add_pref_compression(keygen, "unknown"));
    assert_rnp_success(rnp_op_generate_add_pref_compression(keygen, "zlib"));
    assert_rnp_success(rnp_op_generate_clear_pref_compression(keygen));
    assert_rnp_success(rnp_op_generate_add_pref_compression(keygen, "zip"));
    assert_rnp_success(rnp_op_generate_add_pref_compression(keygen, "zlib"));
    /* preferred hash algorithms */
    assert_rnp_success(rnp_op_generate_clear_pref_hashes(keygen));
    assert_rnp_failure(rnp_op_generate_add_pref_hash(keygen, "unknown"));
    assert_rnp_success(rnp_op_generate_add_pref_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_clear_pref_hashes(keygen));
    assert_rnp_success(rnp_op_generate_add_pref_hash(keygen, "SHA512"));
    assert_rnp_success(rnp_op_generate_add_pref_hash(keygen, "SHA256"));
    /* key expiration */
    assert_rnp_success(rnp_op_generate_set_expiration(keygen, 60 * 60 * 24 * 100));
    assert_rnp_success(rnp_op_generate_set_expiration(keygen, 60 * 60 * 24 * 300));
    /* preferred key server */
    assert_rnp_success(rnp_op_generate_set_pref_keyserver(keygen, NULL));
    assert_rnp_success(rnp_op_generate_set_pref_keyserver(keygen, "hkp://first.server/"));
    assert_rnp_success(rnp_op_generate_set_pref_keyserver(keygen, "hkp://second.server/"));
    /* user id */
    assert_rnp_failure(rnp_op_generate_set_userid(keygen, NULL));
    assert_rnp_success(rnp_op_generate_set_userid(keygen, "userid_cleared"));
    assert_rnp_success(rnp_op_generate_set_userid(keygen, "userid"));
    /* protection */
    assert_rnp_failure(rnp_op_generate_set_protection_cipher(keygen, NULL));
    assert_rnp_failure(rnp_op_generate_set_protection_cipher(keygen, "unknown"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES256"));
    assert_rnp_failure(rnp_op_generate_set_protection_hash(keygen, NULL));
    assert_rnp_failure(rnp_op_generate_set_protection_hash(keygen, "unknown"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA256"));
    assert_rnp_success(rnp_op_generate_set_protection_iterations(keygen, 65536));
    assert_rnp_failure(rnp_op_generate_set_protection_mode(keygen, NULL));
    assert_rnp_failure(rnp_op_generate_set_protection_mode(keygen, "unknown"));
    assert_rnp_success(rnp_op_generate_set_protection_mode(keygen, "cfb"));
    /* now execute keygen operation */
    assert_rnp_success(rnp_op_generate_set_request_password(keygen, true));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* now check key usage */
    bool flag = false;
    assert_rnp_success(rnp_key_allows_usage(key, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(key, "authenticate", &flag));
    assert_false(flag);
    /* check key creation and expiration */
    uint32_t create = 0;
    assert_rnp_success(rnp_key_get_creation(key, &create));
    assert_true((create != 0) && (create <= time(NULL)));
    uint32_t expiry = 0;
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_true(expiry == 60 * 60 * 24 * 300);
    uint32_t till = 0;
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, create + expiry);
    /* check whether key is encrypted */
    assert_rnp_success(rnp_key_is_protected(key, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_is_locked(key, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_unlock(key, "123"));
    assert_rnp_success(rnp_key_is_locked(key, &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_lock(key));

    /* generate DSA subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "DSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1536));
    assert_rnp_success(rnp_op_generate_set_dsa_qbits(keygen, 224));
    /* key flags */
    assert_rnp_failure(rnp_op_generate_add_usage(keygen, "encrypt"));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "certify"));
    /* these should not work for subkey */
    assert_rnp_failure(rnp_op_generate_clear_pref_ciphers(keygen));
    assert_rnp_failure(rnp_op_generate_add_pref_cipher(keygen, "AES256"));
    assert_rnp_failure(rnp_op_generate_clear_pref_compression(keygen));
    assert_rnp_failure(rnp_op_generate_add_pref_compression(keygen, "zlib"));
    assert_rnp_failure(rnp_op_generate_clear_pref_hashes(keygen));
    assert_rnp_failure(rnp_op_generate_add_pref_hash(keygen, "unknown"));
    assert_rnp_failure(rnp_op_generate_set_pref_keyserver(keygen, "hkp://first.server/"));
    assert_rnp_failure(rnp_op_generate_set_userid(keygen, "userid"));
    /* key expiration */
    assert_rnp_success(rnp_op_generate_set_expiration(keygen, 60 * 60 * 24 * 300));
    /* key protection */
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES256"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA256"));
    assert_rnp_success(rnp_op_generate_set_protection_iterations(keygen, 65536));
    assert_rnp_success(rnp_op_generate_set_request_password(keygen, true));
    /* now generate the subkey */
    assert_rnp_success(rnp_op_generate_execute(keygen));
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* now check subkey usage */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    /* check subkey creation and expiration */
    create = 0;
    assert_rnp_success(rnp_key_get_creation(subkey, &create));
    assert_true((create != 0) && (create <= time(NULL)));
    expiry = 0;
    assert_rnp_success(rnp_key_get_expiration(subkey, &expiry));
    assert_true(expiry == 60 * 60 * 24 * 300);
    /* check whether subkey is encrypted */
    assert_rnp_success(rnp_key_is_protected(subkey, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_is_locked(subkey, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_unlock(subkey, "123"));
    assert_rnp_success(rnp_key_is_locked(subkey, &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_lock(subkey));
    /* destroy key handle */
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* generate RSA sign/encrypt subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "sign"));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "encrypt"));
    assert_rnp_success(rnp_op_generate_set_expiration(keygen, 0));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    /* set bits for iterations instead of exact iterations number */
    assert_rnp_success(rnp_op_generate_set_protection_iterations(keygen, 12));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* now check subkey usage */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    /* check whether subkey is encrypted - it should not */
    assert_rnp_success(rnp_key_is_protected(subkey, &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* generate ElGamal subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ELGAMAL"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_failure(rnp_op_generate_add_usage(keygen, "sign"));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "encrypt"));
    assert_rnp_success(rnp_op_generate_set_expiration(keygen, 0));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* now check subkey usage */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* generate ECDSA subkeys for each curve */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_failure(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_failure(rnp_op_generate_set_dsa_qbits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "sign"));
    assert_rnp_failure(rnp_op_generate_add_usage(keygen, "encrypt"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "NIST P-256"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* now check subkey usage */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "NIST P-384"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "NIST P-521"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    if (brainpool_enabled()) {
        assert_rnp_success(rnp_op_generate_set_curve(keygen, "brainpoolP256r1"));
        assert_rnp_success(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
        assert_non_null(subkey);
        assert_rnp_success(rnp_op_generate_destroy(keygen));
        assert_rnp_success(rnp_key_handle_destroy(subkey));
    } else {
        assert_rnp_failure(rnp_op_generate_set_curve(keygen, "brainpoolP256r1"));
        assert_rnp_failure(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_destroy(keygen));
    }

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    if (brainpool_enabled()) {
        assert_rnp_success(rnp_op_generate_set_curve(keygen, "brainpoolP384r1"));
        assert_rnp_success(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
        assert_non_null(subkey);
        assert_rnp_success(rnp_op_generate_destroy(keygen));
        assert_rnp_success(rnp_key_handle_destroy(subkey));
    } else {
        assert_rnp_failure(rnp_op_generate_set_curve(keygen, "brainpoolP384r1"));
        assert_rnp_failure(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_destroy(keygen));
    }

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    if (brainpool_enabled()) {
        assert_rnp_success(rnp_op_generate_set_curve(keygen, "brainpoolP512r1"));
        assert_rnp_success(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
        assert_non_null(subkey);
        assert_rnp_success(rnp_op_generate_destroy(keygen));
        assert_rnp_success(rnp_key_handle_destroy(subkey));
    } else {
        assert_rnp_failure(rnp_op_generate_set_curve(keygen, "brainpoolP512r1"));
        assert_rnp_failure(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_destroy(keygen));
    }

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "secp256k1"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* These curves will not work with ECDSA*/
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "Ed25519"));
    assert_rnp_failure(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_destroy(keygen));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "Curve25519"));
    assert_rnp_failure(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_destroy(keygen));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDSA"));
    if (!sm2_enabled()) {
        assert_rnp_failure(rnp_op_generate_set_curve(keygen, "SM2 P-256"));
    } else {
        assert_rnp_success(rnp_op_generate_set_curve(keygen, "SM2 P-256"));
        assert_rnp_failure(rnp_op_generate_execute(keygen));
    }
    assert_rnp_success(rnp_op_generate_destroy(keygen));

    /* Add EDDSA subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "EDDSA"));
    assert_rnp_failure(rnp_op_generate_set_curve(keygen, "secp256k1"));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "sign"));
    assert_rnp_failure(rnp_op_generate_add_usage(keygen, "encrypt"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* now check subkey usage */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* Add ECDH subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDH"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "NIST P-256"));
    assert_rnp_failure(rnp_op_generate_add_usage(keygen, "sign"));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "encrypt"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* now check subkey usage */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* Add ECDH x25519 subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ECDH"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "Curve25519"));
    assert_rnp_failure(rnp_op_generate_add_usage(keygen, "sign"));
    assert_rnp_success(rnp_op_generate_add_usage(keygen, "encrypt"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* now check subkey usage */
    assert_rnp_success(rnp_key_allows_usage(subkey, "sign", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "certify", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "encrypt", &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_allows_usage(subkey, "authenticate", &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* Add SM2 subkey */
    if (!sm2_enabled()) {
        keygen = NULL;
        assert_rnp_failure(rnp_op_generate_subkey_create(&keygen, ffi, key, "SM2"));
    } else {
        assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "SM2"));
        assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "AES128"));
        assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "SHA1"));
        assert_rnp_success(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
        assert_non_null(subkey);
        assert_rnp_success(rnp_key_handle_destroy(subkey));
    }
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_generate_expiry_32bit)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "123"));

    /* Generate RSA key with creation + expiration > 32 bit */
    rnp_op_generate_t keygen = NULL;
    assert_rnp_success(rnp_op_generate_create(&keygen, ffi, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    /* key expiration */
    assert_rnp_success(rnp_op_generate_set_expiration(keygen, UINT32_MAX));
    /* now execute keygen operation */
    assert_rnp_success(rnp_op_generate_set_request_password(keygen, true));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* check key creation and expiration */
    uint32_t create = 0;
    assert_rnp_success(rnp_key_get_creation(key, &create));
    assert_true((create != 0) && (create <= time(NULL)));
    uint32_t expiry = 0;
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_true(expiry == UINT32_MAX);
    uint32_t till = 0;
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, UINT32_MAX - 1);
    uint64_t till64 = 0;
    assert_rnp_success(rnp_key_valid_till64(key, &till64));
    assert_int_equal(till64, (uint64_t) create + expiry);
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* Load key with creation + expiration == UINT32_MAX */
    assert_true(import_pub_keys(ffi, "data/test_key_edge_cases/key-create-expiry-32bit.asc"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "60eac9ddf0d9ac9f", &key));
    /* check key creation and expiration */
    create = 0;
    assert_rnp_success(rnp_key_get_creation(key, &create));
    assert_int_equal(create, 1619611313);
    expiry = 0;
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_true(expiry == UINT32_MAX - create);
    till = 0;
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, UINT32_MAX - 1);
    till64 = 0;
    assert_rnp_success(rnp_key_valid_till64(key, &till64));
    assert_int_equal(till64, UINT32_MAX);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_generate_algnamecase)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "123"));

    /* Generate RSA key with misc options set */
    rnp_op_generate_t keygen = NULL;
    assert_rnp_success(rnp_op_generate_create(&keygen, ffi, "rsa"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_generate_destroy(keygen));

    /* generate DSA subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "dsa"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1536));
    /* now generate the subkey */
    assert_rnp_success(rnp_op_generate_execute(keygen));
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* destroy key handle */
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* generate ElGamal subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "elgamal"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* generate ECDSA subkeys for each curve */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_failure(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "NIST P-256"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "NIST P-384"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "NIST P-521"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    if (brainpool_enabled()) {
        assert_rnp_success(rnp_op_generate_set_curve(keygen, "brainpoolP256r1"));
        assert_rnp_success(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
        assert_non_null(subkey);
        assert_rnp_success(rnp_op_generate_destroy(keygen));
        assert_rnp_success(rnp_key_handle_destroy(subkey));
    } else {
        assert_rnp_failure(rnp_op_generate_set_curve(keygen, "brainpoolP256r1"));
        assert_rnp_failure(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_destroy(keygen));
    }

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    if (brainpool_enabled()) {
        assert_rnp_success(rnp_op_generate_set_curve(keygen, "brainpoolP384r1"));
        assert_rnp_success(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
        assert_non_null(subkey);
        assert_rnp_success(rnp_op_generate_destroy(keygen));
        assert_rnp_success(rnp_key_handle_destroy(subkey));
    } else {
        assert_rnp_failure(rnp_op_generate_set_curve(keygen, "brainpoolP384r1"));
        assert_rnp_failure(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_destroy(keygen));
    }

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    if (brainpool_enabled()) {
        assert_rnp_success(rnp_op_generate_set_curve(keygen, "brainpoolP512r1"));
        assert_rnp_success(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
        assert_non_null(subkey);
        assert_rnp_success(rnp_op_generate_destroy(keygen));
        assert_rnp_success(rnp_key_handle_destroy(subkey));
    } else {
        assert_rnp_failure(rnp_op_generate_set_curve(keygen, "brainpoolP512r1"));
        assert_rnp_failure(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_destroy(keygen));
    }

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "secp256k1"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* These curves will not work with ECDSA */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "Ed25519"));
    assert_rnp_failure(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_destroy(keygen));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "Curve25519"));
    assert_rnp_failure(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_destroy(keygen));

    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdsa"));
    if (!sm2_enabled()) {
        assert_rnp_failure(rnp_op_generate_set_curve(keygen, "SM2 P-256"));
    } else {
        assert_rnp_success(rnp_op_generate_set_curve(keygen, "SM2 P-256"));
        assert_rnp_failure(rnp_op_generate_execute(keygen));
    }
    assert_rnp_success(rnp_op_generate_destroy(keygen));

    /* Add EDDSA subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "eddsa"));
    assert_rnp_failure(rnp_op_generate_set_curve(keygen, "secp256k1"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* Add ECDH subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdh"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "NIST P-256"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* Add ECDH x25519 subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "ecdh"));
    assert_rnp_success(rnp_op_generate_set_curve(keygen, "Curve25519"));
    assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
    assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(subkey));

    /* Add SM2 subkey */
    if (!sm2_enabled()) {
        keygen = NULL;
        assert_rnp_failure(rnp_op_generate_subkey_create(&keygen, ffi, key, "sm2"));
    } else {
        assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "sm2"));
        assert_rnp_success(rnp_op_generate_set_protection_cipher(keygen, "aes128"));
        assert_rnp_success(rnp_op_generate_set_protection_hash(keygen, "sha1"));
        assert_rnp_success(rnp_op_generate_execute(keygen));
        assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
        assert_non_null(subkey);
        assert_rnp_success(rnp_key_handle_destroy(subkey));
    }
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_generate_protection)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "123"));

    /* Generate key and subkey without protection */
    rnp_op_generate_t keygen = NULL;
    assert_rnp_success(rnp_op_generate_create(&keygen, ffi, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* check whether key is encrypted */
    bool flag = true;
    assert_rnp_success(rnp_key_is_protected(key, &flag));
    assert_false(flag);
    /* generate subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_is_protected(subkey, &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* Generate RSA key with password */
    assert_rnp_success(rnp_op_generate_create(&keygen, ffi, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_set_protection_password(keygen, "password"));
    /* Line below should not change password from 'password' to '123' */
    assert_rnp_success(rnp_op_generate_set_request_password(keygen, true));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    key = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* check whether key is encrypted */
    assert_rnp_success(rnp_key_is_protected(key, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_is_locked(key, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_success(rnp_key_is_locked(key, &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_lock(key));
    /* generate subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_set_protection_password(keygen, "password"));
    /* this should fail since primary key is locked */
    assert_rnp_failure(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_key_unlock(key, "password"));
    /* now it should work */
    assert_rnp_success(rnp_op_generate_execute(keygen));
    subkey = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_is_protected(subkey, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_is_locked(subkey, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_unlock(subkey, "password"));
    assert_rnp_success(rnp_key_is_locked(subkey, &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* Generate RSA key via password request */
    assert_rnp_success(rnp_op_generate_create(&keygen, ffi, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_set_request_password(keygen, true));
    assert_rnp_success(rnp_op_generate_execute(keygen));
    key = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    /* check whether key is encrypted */
    assert_rnp_success(rnp_key_is_protected(key, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_is_locked(key, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_unlock(key, "123"));
    assert_rnp_success(rnp_key_is_locked(key, &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_lock(key));
    /* generate subkey */
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    assert_rnp_success(rnp_op_generate_set_request_password(keygen, true));
    /* this should succeed since password for primary key is returned via provider */
    assert_rnp_success(rnp_op_generate_execute(keygen));
    subkey = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &subkey));
    assert_non_null(subkey);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    assert_rnp_success(rnp_key_is_protected(subkey, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_is_locked(subkey, &flag));
    assert_true(flag);
    assert_rnp_success(rnp_key_unlock(subkey, "123"));
    assert_rnp_success(rnp_key_is_locked(subkey, &flag));
    assert_false(flag);
    assert_rnp_success(rnp_key_handle_destroy(subkey));
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_keygen_json_sub_pass_required)
{
    char *    results = NULL;
    size_t    count = 0;
    rnp_ffi_t ffi = NULL;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, unused_getpasscb, NULL));

    // generate our primary key
    auto json = file_to_str("data/test_ffi_json/generate-primary.json");
    assert_rnp_success(rnp_generate_key_json(ffi, json.c_str(), &results));
    assert_non_null(results);
    // check key counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(1, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(1, count);

    // parse the results JSON
    json_object *parsed_results = json_tokener_parse(results);
    assert_non_null(parsed_results);
    rnp_buffer_destroy(results);
    results = NULL;
    // get a handle+grip for the primary
    rnp_key_handle_t primary = NULL;
    char *           primary_grip = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "primary", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        primary_grip = strdup(json_object_get_string(jsogrip));
        assert_non_null(primary_grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", primary_grip, &primary));
        assert_non_null(primary);
    }
    // cleanup
    json_object_put(parsed_results);
    parsed_results = NULL;

    // protect+lock the primary key
    assert_rnp_success(rnp_key_protect(primary, "pass123", NULL, NULL, NULL, 0));
    assert_rnp_success(rnp_key_lock(primary));
    rnp_key_handle_destroy(primary);
    primary = NULL;

    // load our JSON template
    json = file_to_str("data/test_ffi_json/generate-sub.json");
    // modify our JSON
    {
        // parse
        json_object *jso = json_tokener_parse(json.c_str());
        assert_non_null(jso);
        // find the relevant fields
        json_object *jsosub = NULL;
        json_object *jsoprimary = NULL;
        assert_true(json_object_object_get_ex(jso, "sub", &jsosub));
        assert_non_null(jsosub);
        assert_true(json_object_object_get_ex(jsosub, "primary", &jsoprimary));
        assert_non_null(jsoprimary);
        // replace the placeholder grip with the correct one
        json_object_object_del(jsoprimary, "grip");
        json_object_object_add(jsoprimary, "grip", json_object_new_string(primary_grip));
        assert_int_equal(1, json_object_object_length(jsoprimary));
        json = json_object_to_json_string_ext(jso, JSON_C_TO_STRING_PRETTY);
        assert_false(json.empty());
        json_object_put(jso);
    }
    // cleanup
    rnp_buffer_destroy(primary_grip);
    primary_grip = NULL;

    // generate the subkey (no ffi_string_password_provider, should fail)
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, NULL, NULL));
    assert_rnp_failure(rnp_generate_key_json(ffi, json.c_str(), &results));

    // generate the subkey (wrong pass, should fail)
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "wrong"));
    assert_rnp_failure(rnp_generate_key_json(ffi, json.c_str(), &results));

    // generate the subkey
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "pass123"));
    assert_rnp_success(rnp_generate_key_json(ffi, json.c_str(), &results));
    assert_non_null(results);

    // parse the results JSON
    parsed_results = json_tokener_parse(results);
    assert_non_null(parsed_results);
    rnp_buffer_destroy(results);
    results = NULL;
    // get a handle for the sub
    rnp_key_handle_t sub = NULL;
    {
        json_object *jsokey = NULL;
        assert_int_equal(true, json_object_object_get_ex(parsed_results, "sub", &jsokey));
        assert_non_null(jsokey);
        json_object *jsogrip = NULL;
        assert_int_equal(true, json_object_object_get_ex(jsokey, "grip", &jsogrip));
        assert_non_null(jsogrip);
        const char *grip = json_object_get_string(jsogrip);
        assert_non_null(grip);
        assert_rnp_success(rnp_locate_key(ffi, "grip", grip, &sub));
        assert_non_null(sub);
    }
    // cleanup
    json_object_put(parsed_results);
    parsed_results = NULL;

    // check the key counts
    assert_int_equal(RNP_SUCCESS, rnp_get_public_key_count(ffi, &count));
    assert_int_equal(2, count);
    assert_int_equal(RNP_SUCCESS, rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(2, count);

    // check some key properties
    check_key_properties(sub, false, true, true);

    // cleanup
    rnp_key_handle_destroy(primary);
    rnp_key_handle_destroy(sub);
    rnp_ffi_destroy(ffi);
}

/** get the value of a (potentially nested) field in a json object
 *
 *  Note that this does not support JSON arrays, only objects.
 *
 *  @param jso the json object to search within. This should be an object, not a string,
 *         array, etc.
 *  @param field the field to retrieve. The format is "first.second.third".
 *  @return a pointer to the located json object, or NULL
 **/
static json_object *
get_json_obj(json_object *jso, const char *field)
{
    const char *start = field;
    char        buf[32];

    while (start && *start) {
        const char *end = strchr(start, '.');
        size_t      len = end ? (end - start) : strlen(start);
        if (len >= sizeof(buf)) {
            return NULL;
        }
        memcpy(buf, start, len);
        buf[len] = '\0';

        if (!json_object_object_get_ex(jso, buf, &jso)) {
            return NULL;
        }

        start = end ? end + 1 : NULL;
    };
    return jso;
}

/* This test loads a keyring and converts the keys to JSON,
 * then validates some properties.
 *
 * We could just do a simple strcmp, but that would depend
 * on json-c sorting the keys consistently, across versions,
 * etc.
 */
TEST_F(rnp_tests, test_ffi_key_to_json)
{
    rnp_ffi_t        ffi = NULL;
    char *           pub_format = NULL;
    char *           pub_path = NULL;
    char *           sec_format = NULL;
    char *           sec_path = NULL;
    rnp_key_handle_t key = NULL;
    char *           json = NULL;
    json_object *    jso = NULL;

    // detect the formats+paths
    assert_rnp_success(rnp_detect_homedir_info(
      "data/keyrings/5", &pub_format, &pub_path, &sec_format, &sec_path));
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, pub_format, sec_format));
    // load our keyrings
    assert_true(load_keys_gpg(ffi, pub_path, sec_path));
    // free formats+paths
    rnp_buffer_destroy(pub_format);
    pub_format = NULL;
    rnp_buffer_destroy(pub_path);
    pub_path = NULL;
    rnp_buffer_destroy(sec_format);
    sec_format = NULL;
    rnp_buffer_destroy(sec_path);
    sec_path = NULL;

    // locate key (primary)
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0E33FD46FF10F19C", &key));
    assert_non_null(key);
    // convert to JSON
    json = NULL;
    assert_rnp_success(rnp_key_to_json(key, 0xff, &json));
    assert_non_null(json);
    // parse it back in
    jso = json_tokener_parse(json);
    assert_non_null(jso);
    // validate some properties
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "type")), "ECDSA"));
    assert_int_equal(json_object_get_int(get_json_obj(jso, "length")), 256);
    assert_true(
      rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "curve")), "NIST P-256"));
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "keyid")),
                                 "0E33FD46FF10F19C"));
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "fingerprint")),
                                 "B6B5E497A177551ECB8862200E33FD46FF10F19C"));
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "grip")),
                                 "20A48B3C61525DCDF8B3B9D82C6BBCF4D8BFB5E5"));
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "revoked")), false);
    assert_int_equal(json_object_get_int64(get_json_obj(jso, "creation time")), 1511313500);
    assert_int_equal(json_object_get_int64(get_json_obj(jso, "expiration")), 0);
    // usage
    assert_int_equal(json_object_array_length(get_json_obj(jso, "usage")), 2);
    assert_true(rnp::str_case_eq(
      json_object_get_string(json_object_array_get_idx(get_json_obj(jso, "usage"), 0)),
      "sign"));
    assert_true(rnp::str_case_eq(
      json_object_get_string(json_object_array_get_idx(get_json_obj(jso, "usage"), 1)),
      "certify"));
    // primary key grip
    assert_null(get_json_obj(jso, "primary key grip"));
    // subkey grips
    assert_int_equal(json_object_array_length(get_json_obj(jso, "subkey grips")), 1);
    assert_true(rnp::str_case_eq(
      json_object_get_string(json_object_array_get_idx(get_json_obj(jso, "subkey grips"), 0)),
      "FFFA72FC225214DC712D0127172EE13E88AF93B4"));
    // public key
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "public key.present")), true);
    assert_true(
      rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "public key.mpis.point")),
                       "04B0C6F2F585C1EEDF805C4492CB683839D5EAE6246420780F063D558"
                       "A33F607876BE6F818A665722F8204653CC4DCFAD4F4765521AC8A6E9F"
                       "793CEBAE8600BEEF"));
    // secret key
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "secret key.present")), true);
    assert_true(
      rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "secret key.mpis.x")),
                       "46DE93CA439735F36B9CF228F10D8586DA824D88BBF4E24566D5312D061802C8"));
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "secret key.locked")), false);
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "secret key.protected")),
                     false);
    // userids
    assert_int_equal(json_object_array_length(get_json_obj(jso, "userids")), 1);
    assert_true(rnp::str_case_eq(
      json_object_get_string(json_object_array_get_idx(get_json_obj(jso, "userids"), 0)),
      "test0"));
    // signatures
    assert_int_equal(json_object_array_length(get_json_obj(jso, "signatures")), 1);
    json_object *jsosig = json_object_array_get_idx(get_json_obj(jso, "signatures"), 0);
    assert_int_equal(json_object_get_int(get_json_obj(jsosig, "userid")), 0);
    // TODO: other properties of signature
    // cleanup
    json_object_put(jso);
    rnp_key_handle_destroy(key);
    key = NULL;
    rnp_buffer_destroy(json);
    json = NULL;

    // locate key (sub)
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "074131BC8D16C5C9", &key));
    assert_non_null(key);
    // convert to JSON
    assert_rnp_success(rnp_key_to_json(key, 0xff, &json));
    assert_non_null(json);
    // parse it back in
    jso = json_tokener_parse(json);
    assert_non_null(jso);
    // validate some properties
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "type")), "ECDH"));
    assert_int_equal(json_object_get_int(get_json_obj(jso, "length")), 256);
    assert_true(
      rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "curve")), "NIST P-256"));
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "keyid")),
                                 "074131BC8D16C5C9"));
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "fingerprint")),
                                 "481E6A41B10ECD71A477DB02074131BC8D16C5C9"));
    // ECDH-specific
    assert_true(
      rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "kdf hash")), "SHA256"));
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "key wrap cipher")),
                                 "AES128"));
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "grip")),
                                 "FFFA72FC225214DC712D0127172EE13E88AF93B4"));
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "revoked")), false);
    assert_int_equal(json_object_get_int64(get_json_obj(jso, "creation time")), 1511313500);
    assert_int_equal(json_object_get_int64(get_json_obj(jso, "expiration")), 0);
    // usage
    assert_int_equal(json_object_array_length(get_json_obj(jso, "usage")), 1);
    assert_true(rnp::str_case_eq(
      json_object_get_string(json_object_array_get_idx(get_json_obj(jso, "usage"), 0)),
      "encrypt"));
    // primary key grip
    assert_true(rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "primary key grip")),
                                 "20A48B3C61525DCDF8B3B9D82C6BBCF4D8BFB5E5"));
    // subkey grips
    assert_null(get_json_obj(jso, "subkey grips"));
    // public key
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "public key.present")), true);
    assert_true(rnp::str_case_eq(
      json_object_get_string(get_json_obj(jso, "public key.mpis.point")),
      "04E2746BA4D180011B17A6909EABDBF2F3733674FBE00B20A3B857C2597233651544150B"
      "896BCE7DCDF47C49FC1E12D5AD86384D26336A48A18845940A3F65F502"));
    // secret key
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "secret key.present")), true);
    assert_true(
      rnp::str_case_eq(json_object_get_string(get_json_obj(jso, "secret key.mpis.x")),
                       "DF8BEB7272117AD7AFE2B7E882453113059787FBC785C82F78624EE7EF2117FB"));
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "secret key.locked")), false);
    assert_int_equal(json_object_get_boolean(get_json_obj(jso, "secret key.protected")),
                     false);
    // userids
    assert_null(get_json_obj(jso, "userids"));
    // signatures
    assert_int_equal(json_object_array_length(get_json_obj(jso, "signatures")), 1);
    jsosig = json_object_array_get_idx(get_json_obj(jso, "signatures"), 0);
    assert_null(get_json_obj(jsosig, "userid"));
    // TODO: other properties of signature
    // cleanup
    json_object_put(jso);
    rnp_key_handle_destroy(key);
    rnp_buffer_destroy(json);

    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_iter)
{
    rnp_ffi_t ffi = NULL;
    char *    pub_format = NULL;
    char *    pub_path = NULL;
    char *    sec_format = NULL;
    char *    sec_path = NULL;

    // detect the formats+paths
    assert_rnp_success(rnp_detect_homedir_info(
      "data/keyrings/1", &pub_format, &pub_path, &sec_format, &sec_path));
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, pub_format, sec_format));

    // test invalid identifier type
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_failure(rnp_identifier_iterator_create(ffi, &it, "keyidz"));
        assert_null(it);
    }

    // test empty rings
    // keyid
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "keyid"));
        assert_non_null(it);
        const char *ident = NULL;
        assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
        assert_null(ident);
        assert_rnp_success(rnp_identifier_iterator_destroy(it));
    }
    // grip
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "grip"));
        assert_non_null(it);
        const char *ident = NULL;
        assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
        assert_null(ident);
        assert_rnp_success(rnp_identifier_iterator_destroy(it));
    }
    // userid
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "userid"));
        assert_non_null(it);
        const char *ident = NULL;
        assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
        assert_null(ident);
        assert_rnp_success(rnp_identifier_iterator_destroy(it));
    }
    // fingerprint
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "fingerprint"));
        assert_non_null(it);
        const char *ident = NULL;
        assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
        assert_null(ident);
        assert_rnp_success(rnp_identifier_iterator_destroy(it));
    }

    // load our keyrings
    assert_true(load_keys_gpg(ffi, pub_path, sec_path));
    // free formats+paths
    rnp_buffer_destroy(pub_format);
    pub_format = NULL;
    rnp_buffer_destroy(pub_path);
    pub_path = NULL;
    rnp_buffer_destroy(sec_format);
    sec_format = NULL;
    rnp_buffer_destroy(sec_path);
    sec_path = NULL;

    // keyid
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "keyid"));
        assert_non_null(it);
        {
            static const char *expected[] = {"7BC6709B15C23A4A",
                                             "1ED63EE56FADC34D",
                                             "1D7E8A5393C997A8",
                                             "8A05B89FAD5ADED1",
                                             "2FCADF05FFA501BB",
                                             "54505A936A4A970E",
                                             "326EF111425D14A5"};
            size_t             i = 0;
            const char *       ident = NULL;
            do {
                ident = NULL;
                assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
                if (ident) {
                    assert_true(rnp::str_case_eq(expected[i], ident));
                    i++;
                }
            } while (ident);
            assert_int_equal(i, ARRAY_SIZE(expected));
        }
        assert_rnp_success(rnp_identifier_iterator_destroy(it));
    }

    // grip
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "grip"));
        assert_non_null(it);
        {
            static const char *expected[] = {"66D6A0800A3FACDE0C0EB60B16B3669ED380FDFA",
                                             "D9839D61EDAF0B3974E0A4A341D6E95F3479B9B7",
                                             "B1CC352FEF9A6BD4E885B5351840EF9306D635F0",
                                             "E7C8860B70DC727BED6DB64C633683B41221BB40",
                                             "B2A7F6C34AA2C15484783E9380671869A977A187",
                                             "43C01D6D96BE98C3C87FE0F175870ED92DE7BE45",
                                             "8082FE753013923972632550838A5F13D81F43B9"};
            size_t             i = 0;
            const char *       ident = NULL;
            do {
                ident = NULL;
                assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
                if (ident) {
                    assert_true(rnp::str_case_eq(expected[i], ident));
                    i++;
                }
            } while (ident);
            assert_int_equal(i, ARRAY_SIZE(expected));
        }
        assert_rnp_success(rnp_identifier_iterator_destroy(it));
    }

    // userid
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "userid"));
        assert_non_null(it);
        {
            static const char *expected[] = {
              "key0-uid0", "key0-uid1", "key0-uid2", "key1-uid0", "key1-uid2", "key1-uid1"};
            size_t      i = 0;
            const char *ident = NULL;
            do {
                ident = NULL;
                assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
                if (ident) {
                    assert_true(rnp::str_case_eq(expected[i], ident));
                    i++;
                }
            } while (ident);
            assert_int_equal(i, ARRAY_SIZE(expected));
        }
        assert_rnp_success(rnp_identifier_iterator_destroy(it));
    }

    // fingerprint
    {
        rnp_identifier_iterator_t it = NULL;
        assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "fingerprint"));
        assert_non_null(it);
        {
            static const char *expected[] = {"E95A3CBF583AA80A2CCC53AA7BC6709B15C23A4A",
                                             "E332B27CAF4742A11BAA677F1ED63EE56FADC34D",
                                             "C5B15209940A7816A7AF3FB51D7E8A5393C997A8",
                                             "5CD46D2A0BD0B8CFE0B130AE8A05B89FAD5ADED1",
                                             "BE1C4AB951F4C2F6B604C7F82FCADF05FFA501BB",
                                             "A3E94DE61A8CB229413D348E54505A936A4A970E",
                                             "57F8ED6E5C197DB63C60FFAF326EF111425D14A5"};
            size_t             i = 0;
            const char *       ident = NULL;
            do {
                ident = NULL;
                assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
                if (ident) {
                    assert_true(rnp::str_case_eq(expected[i], ident));
                    i++;
                }
            } while (ident);
            assert_int_equal(i, ARRAY_SIZE(expected));
        }
        assert_rnp_success(rnp_identifier_iterator_destroy(it));
    }

    // cleanup
    rnp_ffi_destroy(ffi);
}

void
check_loaded_keys(const char *                    format,
                  bool                            armored,
                  uint8_t *                       buf,
                  size_t                          buf_len,
                  const char *                    id_type,
                  const std::vector<std::string> &expected_ids,
                  bool                            secret)
{
    rnp_ffi_t                 ffi = NULL;
    rnp_input_t               input = NULL;
    rnp_identifier_iterator_t it = NULL;
    const char *              identifier = NULL;

    if (armored) {
        assert_memory_equal("-----", buf, 5);
    } else {
        assert_memory_not_equal("-----", buf, 5);
    }

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, format, format));

    // load our keyrings
    assert_rnp_success(rnp_input_from_memory(&input, buf, buf_len, true));
    assert_rnp_success(rnp_load_keys(
      ffi, format, input, secret ? RNP_LOAD_SAVE_SECRET_KEYS : RNP_LOAD_SAVE_PUBLIC_KEYS));
    rnp_input_destroy(input);
    input = NULL;

    std::vector<std::string> ids;
    assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, id_type));
    do {
        identifier = NULL;
        assert_rnp_success(rnp_identifier_iterator_next(it, &identifier));
        if (identifier) {
            rnp_key_handle_t key = NULL;
            bool             expected_secret = secret;
            bool             expected_public = !secret;
            bool             result;
            assert_rnp_success(rnp_locate_key(ffi, id_type, identifier, &key));
            assert_non_null(key);
            assert_rnp_success(rnp_key_have_secret(key, &result));
            assert_int_equal(result, expected_secret);
            assert_rnp_success(rnp_key_have_public(key, &result));
            assert_int_equal(result, expected_public);
            assert_rnp_success(rnp_key_handle_destroy(key));
            ids.push_back(identifier);
        }
    } while (identifier);
    assert_true(ids == expected_ids);
    rnp_identifier_iterator_destroy(it);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_export)
{
    rnp_ffi_t        ffi = NULL;
    rnp_output_t     output = NULL;
    rnp_key_handle_t key = NULL;
    uint8_t *        buf = NULL;
    size_t           buf_len = 0;

    // setup FFI
    test_ffi_init(&ffi);

    // primary pub only
    {
        // locate key
        key = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "keyid", "2FCADF05FFA501BB", &key));
        assert_non_null(key);

        // create output
        output = NULL;
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_non_null(output);

        // export
        assert_rnp_success(rnp_key_export(key, output, RNP_KEY_EXPORT_PUBLIC));

        // get output
        buf = NULL;
        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
        assert_non_null(buf);

        // check results
        check_loaded_keys("GPG", false, buf, buf_len, "keyid", {"2FCADF05FFA501BB"}, false);

        // cleanup
        rnp_output_destroy(output);
        rnp_key_handle_destroy(key);
    }

    // primary sec only (armored)
    {
        // locate key
        key = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "keyid", "2FCADF05FFA501BB", &key));
        assert_non_null(key);

        // create output
        output = NULL;
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_non_null(output);

        // export
        assert_rnp_success(
          rnp_key_export(key, output, RNP_KEY_EXPORT_SECRET | RNP_KEY_EXPORT_ARMORED));

        // get output
        buf = NULL;
        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
        assert_non_null(buf);

        // check results
        check_loaded_keys("GPG", true, buf, buf_len, "keyid", {"2FCADF05FFA501BB"}, true);

        // cleanup
        rnp_output_destroy(output);
        rnp_key_handle_destroy(key);
    }

    // primary pub and subs
    {
        // locate key
        key = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "keyid", "2FCADF05FFA501BB", &key));
        assert_non_null(key);

        // create output
        output = NULL;
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_non_null(output);

        // export
        assert_rnp_success(
          rnp_key_export(key, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS));

        // get output
        buf = NULL;
        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
        assert_non_null(buf);

        // check results
        check_loaded_keys("GPG",
                          false,
                          buf,
                          buf_len,
                          "keyid",
                          {"2FCADF05FFA501BB", "54505A936A4A970E", "326EF111425D14A5"},
                          false);

        // cleanup
        rnp_output_destroy(output);
        rnp_key_handle_destroy(key);
    }

    // primary sec and subs (armored)
    {
        // locate key
        key = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "keyid", "2FCADF05FFA501BB", &key));
        assert_non_null(key);

        // create output
        output = NULL;
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_non_null(output);

        // export
        assert_rnp_success(rnp_key_export(key,
                                          output,
                                          RNP_KEY_EXPORT_SECRET | RNP_KEY_EXPORT_SUBKEYS |
                                            RNP_KEY_EXPORT_ARMORED));

        // get output
        buf = NULL;
        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
        assert_non_null(buf);

        // check results
        check_loaded_keys("GPG",
                          true,
                          buf,
                          buf_len,
                          "keyid",
                          {"2FCADF05FFA501BB", "54505A936A4A970E", "326EF111425D14A5"},
                          true);

        // cleanup
        rnp_output_destroy(output);
        rnp_key_handle_destroy(key);
    }

    // sub pub
    {
        // locate key
        key = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "keyid", "54505A936A4A970E", &key));
        assert_non_null(key);

        // create output
        output = NULL;
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_non_null(output);

        // export
        assert_rnp_success(
          rnp_key_export(key, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_ARMORED));

        // get output
        buf = NULL;
        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
        assert_non_null(buf);

        // check results
        check_loaded_keys(
          "GPG", true, buf, buf_len, "keyid", {"2FCADF05FFA501BB", "54505A936A4A970E"}, false);

        // cleanup
        rnp_output_destroy(output);
        rnp_key_handle_destroy(key);
    }

    // sub sec
    {
        // locate key
        key = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "keyid", "54505A936A4A970E", &key));
        assert_non_null(key);

        // create output
        output = NULL;
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_non_null(output);

        // export
        assert_rnp_success(
          rnp_key_export(key, output, RNP_KEY_EXPORT_SECRET | RNP_KEY_EXPORT_ARMORED));

        // get output
        buf = NULL;
        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
        assert_non_null(buf);

        // check results
        check_loaded_keys(
          "GPG", true, buf, buf_len, "keyid", {"2FCADF05FFA501BB", "54505A936A4A970E"}, true);

        // cleanup
        rnp_output_destroy(output);
        rnp_key_handle_destroy(key);
    }

    // cleanup
    rnp_ffi_destroy(ffi);
}

static bool
check_import_keys_ex(rnp_ffi_t     ffi,
                     json_object **jso,
                     uint32_t      flags,
                     rnp_input_t   input,
                     size_t        rescount,
                     size_t        pubcount,
                     size_t        seccount)
{
    bool         res = false;
    char *       keys = NULL;
    size_t       keycount = 0;
    json_object *keyarr = NULL;
    *jso = NULL;

    if (rnp_import_keys(ffi, input, flags, &keys)) {
        goto done;
    }
    if (rnp_get_public_key_count(ffi, &keycount) || (keycount != pubcount)) {
        goto done;
    }
    if (rnp_get_secret_key_count(ffi, &keycount) || (keycount != seccount)) {
        goto done;
    }
    if (!keys) {
        goto done;
    }

    *jso = json_tokener_parse(keys);
    if (!jso) {
        goto done;
    }
    if (!json_object_is_type(*jso, json_type_object)) {
        goto done;
    }
    if (!json_object_object_get_ex(*jso, "keys", &keyarr)) {
        goto done;
    }
    if (!json_object_is_type(keyarr, json_type_array)) {
        goto done;
    }
    if ((size_t) json_object_array_length(keyarr) != rescount) {
        goto done;
    }
    res = true;
done:
    if (!res) {
        json_object_put(*jso);
        *jso = NULL;
    }
    rnp_buffer_destroy(keys);
    return res;
}

static bool
check_import_keys(rnp_ffi_t     ffi,
                  json_object **jso,
                  const char *  keypath,
                  size_t        rescount,
                  size_t        pubcount,
                  size_t        seccount)
{
    rnp_input_t input = NULL;

    if (rnp_input_from_path(&input, keypath)) {
        return false;
    }
    bool res = check_import_keys_ex(ffi,
                                    jso,
                                    RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS,
                                    input,
                                    rescount,
                                    pubcount,
                                    seccount);
    rnp_input_destroy(input);
    return res;
}

static bool
check_key_status(
  json_object *jso, size_t idx, const char *pub, const char *sec, const char *fp)
{
    if (!jso) {
        return false;
    }
    if (!json_object_is_type(jso, json_type_object)) {
        return false;
    }
    json_object *keys = NULL;
    if (!json_object_object_get_ex(jso, "keys", &keys)) {
        return false;
    }
    if (!json_object_is_type(keys, json_type_array)) {
        return false;
    }
    json_object *key = json_object_array_get_idx(keys, idx);
    if (!json_object_is_type(key, json_type_object)) {
        return false;
    }
    json_object *fld = NULL;
    if (!json_object_object_get_ex(key, "public", &fld)) {
        return false;
    }
    if (strcmp(json_object_get_string(fld), pub) != 0) {
        return false;
    }
    if (!json_object_object_get_ex(key, "secret", &fld)) {
        return false;
    }
    if (strcmp(json_object_get_string(fld), sec) != 0) {
        return false;
    }
    if (!json_object_object_get_ex(key, "fingerprint", &fld)) {
        return false;
    }
    if (strcmp(json_object_get_string(fld), fp) != 0) {
        return false;
    }
    return true;
}

TEST_F(rnp_tests, test_ffi_keys_import)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // some edge cases
    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_key_merge/key-both.asc"));
    assert_rnp_failure(rnp_import_keys(NULL, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    assert_rnp_failure(rnp_import_keys(ffi, NULL, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    assert_rnp_failure(rnp_import_keys(ffi, input, 0, NULL));
    assert_rnp_failure(rnp_import_keys(ffi, input, 0x31, NULL));
    // load just public keys
    assert_rnp_success(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    rnp_input_destroy(input);
    size_t keycount = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 3);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    // load just secret keys from pubkey file
    assert_true(import_sec_keys(ffi, "data/test_stream_key_merge/key-pub.asc"));
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);
    // load both public and secret keys by specifying just secret (it will create pub part)
    assert_true(import_sec_keys(ffi, "data/test_stream_key_merge/key-sec.asc"));
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 3);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 3);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    // import just a public key without subkeys
    json_object *jso = NULL;
    assert_true(check_import_keys(
      ffi, &jso, "data/test_stream_key_merge/key-pub-just-key.pgp", 1, 1, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "090bd712a1166be572252c3c9747d2a6b3a63124"));
    json_object_put(jso);
    // import just subkey 1
    assert_true(check_import_keys(
      ffi, &jso, "data/test_stream_key_merge/key-pub-just-subkey-1.pgp", 1, 2, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "51b45a4c74917272e4e34180af1114a47f5f5b28"));
    json_object_put(jso);
    // import just subkey 2 without sigs
    assert_true(check_import_keys(
      ffi, &jso, "data/test_stream_key_merge/key-pub-just-subkey-2-no-sigs.pgp", 1, 3, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "5fe514a54816e1b331686c2c16cd16f267ccdd4f"));
    json_object_put(jso);
    // import subkey 2 with sigs
    assert_true(check_import_keys(
      ffi, &jso, "data/test_stream_key_merge/key-pub-just-subkey-2.pgp", 1, 3, 0));
    assert_true(
      check_key_status(jso, 0, "updated", "none", "5fe514a54816e1b331686c2c16cd16f267ccdd4f"));
    json_object_put(jso);
    // import first uid
    assert_true(
      check_import_keys(ffi, &jso, "data/test_stream_key_merge/key-pub-uid-1.pgp", 1, 3, 0));
    assert_true(
      check_key_status(jso, 0, "updated", "none", "090bd712a1166be572252c3c9747d2a6b3a63124"));
    json_object_put(jso);
    // import the whole key
    assert_true(
      check_import_keys(ffi, &jso, "data/test_stream_key_merge/key-pub.pgp", 3, 3, 0));
    assert_true(
      check_key_status(jso, 0, "updated", "none", "090bd712a1166be572252c3c9747d2a6b3a63124"));
    assert_true(check_key_status(
      jso, 1, "unchanged", "none", "51b45a4c74917272e4e34180af1114a47f5f5b28"));
    assert_true(check_key_status(
      jso, 2, "unchanged", "none", "5fe514a54816e1b331686c2c16cd16f267ccdd4f"));
    json_object_put(jso);
    // import the first secret subkey
    assert_true(check_import_keys(
      ffi, &jso, "data/test_stream_key_merge/key-sec-just-subkey-1.pgp", 1, 3, 1));
    assert_true(check_key_status(
      jso, 0, "unchanged", "new", "51b45a4c74917272e4e34180af1114a47f5f5b28"));
    json_object_put(jso);
    // import the second secret subkey
    assert_true(check_import_keys(
      ffi, &jso, "data/test_stream_key_merge/key-sec-just-subkey-2-no-sigs.pgp", 1, 3, 2));
    assert_true(check_key_status(
      jso, 0, "unchanged", "new", "5fe514a54816e1b331686c2c16cd16f267ccdd4f"));
    json_object_put(jso);
    // import the whole secret key
    assert_true(
      check_import_keys(ffi, &jso, "data/test_stream_key_merge/key-sec.pgp", 3, 3, 3));
    assert_true(check_key_status(
      jso, 0, "unchanged", "new", "090bd712a1166be572252c3c9747d2a6b3a63124"));
    assert_true(check_key_status(
      jso, 1, "unchanged", "unchanged", "51b45a4c74917272e4e34180af1114a47f5f5b28"));
    assert_true(check_key_status(
      jso, 2, "unchanged", "unchanged", "5fe514a54816e1b331686c2c16cd16f267ccdd4f"));
    json_object_put(jso);
    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_elgamal4096)
{
    rnp_ffi_t ffi = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* load public key */
    json_object *jso = NULL;
    assert_true(
      check_import_keys(ffi, &jso, "data/test_key_edge_cases/key-eg-4096-pub.pgp", 2, 2, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "6541db10cdfcdba89db2dffea8f0408eb3369d8e"));
    assert_true(
      check_key_status(jso, 1, "new", "none", "c402a09b74acd0c11efc0527a3d630b457a0b15b"));
    json_object_put(jso);
    /* load secret key */
    assert_true(
      check_import_keys(ffi, &jso, "data/test_key_edge_cases/key-eg-4096-sec.pgp", 2, 2, 2));
    assert_true(check_key_status(
      jso, 0, "unchanged", "new", "6541db10cdfcdba89db2dffea8f0408eb3369d8e"));
    assert_true(check_key_status(
      jso, 1, "unchanged", "new", "c402a09b74acd0c11efc0527a3d630b457a0b15b"));
    json_object_put(jso);
    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_malformed_keys_import)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* import keys with bad key0-uid0 certification, first without flag */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/pubring-malf-cert.pgp"));
    assert_rnp_failure(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    rnp_input_destroy(input);
    size_t keycount = 255;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);
    /* now try with RNP_LOAD_SAVE_PERMISSIVE */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/pubring-malf-cert.pgp"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_PERMISSIVE, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 7);
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_non_null(key);
    size_t uidcount = 255;
    assert_rnp_success(rnp_key_get_uid_count(key, &uidcount));
    assert_int_equal(uidcount, 3);
    size_t subcount = 255;
    assert_rnp_success(rnp_key_get_subkey_count(key, &subcount));
    assert_int_equal(subcount, 3);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2fcadf05ffa501bb", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* import keys with bad key0-sub0 binding */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/pubring-malf-key0-sub0-bind.pgp"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_PERMISSIVE, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 7);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_non_null(key);
    uidcount = 255;
    assert_rnp_success(rnp_key_get_uid_count(key, &uidcount));
    assert_int_equal(uidcount, 3);
    subcount = 255;
    assert_rnp_success(rnp_key_get_subkey_count(key, &subcount));
    assert_int_equal(subcount, 3);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2fcadf05ffa501bb", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* import keys with bad key0-sub0 packet */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/pubring-malf-key0-sub0.pgp"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_PERMISSIVE, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 6);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_non_null(key);
    uidcount = 255;
    assert_rnp_success(rnp_key_get_uid_count(key, &uidcount));
    assert_int_equal(uidcount, 3);
    subcount = 255;
    assert_rnp_success(rnp_key_get_subkey_count(key, &subcount));
    assert_int_equal(subcount, 2);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2fcadf05ffa501bb", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* import keys with bad key0 packet */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/pubring-malf-key0.pgp"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_PERMISSIVE, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 3);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2fcadf05ffa501bb", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* import secret keys with bad key1 packet - public should be added as well */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/secring-malf-key1.pgp"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_PERMISSIVE, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 7);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 4);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_non_null(key);
    bool secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "326ef111425d14a5", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_false(secret);
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* import secret keys with bad key0 packet */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/secring-malf-key0.pgp"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_PERMISSIVE, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 7);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 7);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid2", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "326ef111425d14a5", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    assert_rnp_success(rnp_key_handle_destroy(key));

    /* import unprotected secret key with wrong crc */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_false(
      import_sec_keys(ffi, "data/test_key_edge_cases/key-25519-tweaked-wrong-crc.asc"));
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);

    /* cleanup */
    rnp_ffi_destroy(ffi);
}

#if defined(ENABLE_CRYPTO_REFRESH)
TEST_F(rnp_tests, test_ffi_v6_sig_subpackets)
{
    rnp_ffi_t ffi = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, unused_getpasscb, NULL));

    rnp_op_generate_t op = NULL;
    assert_rnp_success(rnp_op_generate_create(&op, ffi, "EDDSA"));
    assert_rnp_success(rnp_op_generate_set_v6_key(op));
    assert_rnp_success(rnp_op_generate_set_userid(op, "test"));
    assert_rnp_success(rnp_op_generate_add_usage(op, "sign"));
    assert_rnp_success(rnp_op_generate_add_usage(op, "certify"));
    assert_rnp_success(rnp_op_generate_set_expiration(op, 0));
    assert_rnp_success(rnp_op_generate_execute(op));
    rnp_key_handle_t primary = NULL;
    assert_rnp_success(rnp_op_generate_get_key(op, &primary));

    assert_true(primary->pub->get_sig(0).sig.has_subpkt(
      PGP_SIG_SUBPKT_ISSUER_FPR, false)); // MUST NOT have issuer key id extension
    assert_false(primary->pub->get_sig(0).sig.has_subpkt(
      PGP_SIG_SUBPKT_ISSUER_KEY_ID, false)); // SHOULD have issuer fingerprint

    rnp_key_handle_destroy(primary);
    rnp_op_generate_destroy(op);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_v6_cert_import)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;
    size_t      keycount = 255;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_v6_valid_data/transferable_pubkey_v6.asc"));
    assert_rnp_success(
      rnp_import_keys(ffi,
                      input,
                      RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SINGLE | RNP_LOAD_SAVE_BASE64,
                      NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 2);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);

    /* check that fingerprint is correct by checking the fingerprint in the signature (coming
      from the correct input data) vs the computed fingerprint value of the primary key.
      Issuer fingerprint is the priamry's key fingerprint for the primary and its subkeys */
    pgp::Fingerprint primary_fp;
    for (rnp::Key key : ffi->pubring->keys) {
        if (key.is_primary()) {
            primary_fp = key.fp();
        }
    }

    for (rnp::Key key : ffi->pubring->keys) {
        /* get first sig and its issuer fpr subpacket */
        rnp::Signature subsig = key.get_sig(0);
        auto issuer_fpr = subsig.sig.get_subpkt(pgp::pkt::sigsub::Type::IssuerFingerprint);
        assert_non_null(issuer_fpr);

        /* check that fingerprints match */
        assert_int_equal(key.fp().size(), PGP_FINGERPRINT_V6_SIZE);
        assert_memory_equal(issuer_fpr->data().data() + 1,
                            primary_fp.data(),
                            primary_fp.size()); // first byte in data is the version - skip
    }
    rnp_ffi_destroy(ffi);
}

#if defined(ENABLE_PQC)
// NOTE: this tests ML-KEM-ipd test vectors
// The final implementation of the PQC draft implementation will use the final NIST standard.
TEST_F(rnp_tests, test_ffi_pqc_certs)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;
    size_t      keycount = 255;

    /* Public Key */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/draft-ietf-openpgp-pqc/v6-eddsa-mlkem.pub.asc"));
    assert_rnp_success(
      rnp_import_keys(ffi,
                      input,
                      RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SINGLE | RNP_LOAD_SAVE_BASE64,
                      NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 2);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);
    rnp_ffi_destroy(ffi);

    /* Private Key */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/draft-ietf-openpgp-pqc/v6-eddsa-mlkem.sec.asc"));
    assert_rnp_success(
      rnp_import_keys(ffi,
                      input,
                      RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_SINGLE | RNP_LOAD_SAVE_BASE64,
                      NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 2);
    rnp_ffi_destroy(ffi);
}

#endif

TEST_F(rnp_tests, test_ffi_v6_seckey_import)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;
    size_t      keycount = 255;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_v6_valid_data/transferable_seckey_v6.asc"));
    assert_rnp_success(
      rnp_import_keys(ffi,
                      input,
                      RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_SINGLE | RNP_LOAD_SAVE_BASE64,
                      NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &keycount));
    assert_int_equal(keycount, 2);
    rnp_ffi_destroy(ffi);
}
#endif

TEST_F(rnp_tests, test_ffi_iterated_key_import)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;
    uint32_t    flags =
      RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_SINGLE;

    /* two primary keys with attached subkeys in binary keyring */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_input_from_path(&input, "data/keyrings/1/pubring.gpg"));
    json_object *jso = NULL;
    assert_true(check_import_keys_ex(ffi, &jso, flags, input, 4, 4, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "e95a3cbf583aa80a2ccc53aa7bc6709b15c23a4a"));
    assert_true(
      check_key_status(jso, 1, "new", "none", "e332b27caf4742a11baa677f1ed63ee56fadc34d"));
    assert_true(
      check_key_status(jso, 2, "new", "none", "c5b15209940a7816a7af3fb51d7e8a5393c997a8"));
    assert_true(
      check_key_status(jso, 3, "new", "none", "5cd46d2a0bd0b8cfe0b130ae8a05b89fad5aded1"));
    json_object_put(jso);

    assert_true(check_import_keys_ex(ffi, &jso, flags, input, 3, 7, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "be1c4ab951f4c2f6b604c7f82fcadf05ffa501bb"));
    assert_true(
      check_key_status(jso, 1, "new", "none", "a3e94de61a8cb229413d348e54505a936a4a970e"));
    assert_true(
      check_key_status(jso, 2, "new", "none", "57f8ed6e5c197db63c60ffaf326ef111425d14a5"));
    json_object_put(jso);

    char *results = NULL;
    assert_int_equal(RNP_ERROR_EOF, rnp_import_keys(ffi, input, flags, &results));
    assert_null(results);
    rnp_input_destroy(input);

    /* public + secret key, armored separately */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_key_merge/key-both.asc"));
    assert_true(check_import_keys_ex(ffi, &jso, flags, input, 3, 3, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "090bd712a1166be572252c3c9747d2a6b3a63124"));
    assert_true(
      check_key_status(jso, 1, "new", "none", "51b45a4c74917272e4e34180af1114a47f5f5b28"));
    assert_true(
      check_key_status(jso, 2, "new", "none", "5fe514a54816e1b331686c2c16cd16f267ccdd4f"));
    json_object_put(jso);

    assert_true(check_import_keys_ex(ffi, &jso, flags, input, 3, 3, 3));
    assert_true(check_key_status(
      jso, 0, "unchanged", "new", "090bd712a1166be572252c3c9747d2a6b3a63124"));
    assert_true(check_key_status(
      jso, 1, "unchanged", "new", "51b45a4c74917272e4e34180af1114a47f5f5b28"));
    assert_true(check_key_status(
      jso, 2, "unchanged", "new", "5fe514a54816e1b331686c2c16cd16f267ccdd4f"));
    json_object_put(jso);

    assert_int_equal(RNP_ERROR_EOF, rnp_import_keys(ffi, input, flags, &results));
    assert_null(results);
    rnp_input_destroy(input);

    /* public keyring, enarmored */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(rnp_input_from_path(&input, "data/keyrings/1/pubring.gpg.asc"));
    flags |= RNP_LOAD_SAVE_PERMISSIVE;
    assert_true(check_import_keys_ex(ffi, &jso, flags, input, 4, 4, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "e95a3cbf583aa80a2ccc53aa7bc6709b15c23a4a"));
    assert_true(
      check_key_status(jso, 1, "new", "none", "e332b27caf4742a11baa677f1ed63ee56fadc34d"));
    assert_true(
      check_key_status(jso, 2, "new", "none", "c5b15209940a7816a7af3fb51d7e8a5393c997a8"));
    assert_true(
      check_key_status(jso, 3, "new", "none", "5cd46d2a0bd0b8cfe0b130ae8a05b89fad5aded1"));
    json_object_put(jso);

    assert_true(check_import_keys_ex(ffi, &jso, flags, input, 3, 7, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "be1c4ab951f4c2f6b604c7f82fcadf05ffa501bb"));
    assert_true(
      check_key_status(jso, 1, "new", "none", "a3e94de61a8cb229413d348e54505a936a4a970e"));
    assert_true(
      check_key_status(jso, 2, "new", "none", "57f8ed6e5c197db63c60ffaf326ef111425d14a5"));
    json_object_put(jso);

    results = NULL;
    assert_int_equal(RNP_ERROR_EOF, rnp_import_keys(ffi, input, flags, &results));
    assert_null(results);
    rnp_input_destroy(input);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_stripped_keys_import)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* load stripped key as keyring */
    assert_true(load_keys_gpg(ffi, "data/test_key_validity/case8/pubring.gpg"));
    /* validate signatures - must succeed */
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_validity/case8/message.txt.asc"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_op_verify_signature_t sig;
    /* signature 1 - by primary key */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    /* signature 2 - by subkey */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 1, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    rnp_op_verify_destroy(verify);

    /* load stripped key by parts via import */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/primary.pgp"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/subkey.pgp"));
    /* validate signatures - must be valid */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_validity/case8/message.txt.asc"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    /* signature 1 - by primary key */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    /* signature 2 - by subkey */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 1, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    rnp_op_verify_destroy(verify);

    /* load stripped key with subkey first */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/subkey.pgp"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/primary.pgp"));
    /* validate signatures - must be valid */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_validity/case8/message.txt.asc"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    /* signature 1 - by primary key */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    /* signature 2 - by subkey */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 1, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    rnp_op_verify_destroy(verify);

    /* load stripped key without subkey binding */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/primary.pgp"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/subkey-no-sig.pgp"));
    /* validate signatures - must be invalid */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_validity/case8/message.txt.asc"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_int_equal(rnp_op_verify_execute(verify), RNP_ERROR_SIGNATURE_INVALID);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    /* signature 1 - by primary key */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_int_equal(rnp_op_verify_signature_get_status(sig), RNP_ERROR_SIGNATURE_INVALID);
    /* signature 2 - by subkey */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 1, &sig));
    assert_int_equal(rnp_op_verify_signature_get_status(sig), RNP_ERROR_SIGNATURE_INVALID);
    rnp_op_verify_destroy(verify);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_import_edge_cases)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* key with empty packets - must fail with bad format */
    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/key-empty-packets.pgp"));
    char *results = NULL;
    assert_int_equal(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, &results),
                     RNP_ERROR_BAD_FORMAT);
    assert_null(results);
    rnp_input_destroy(input);

    /* key with empty uid - must succeed */
    json_object *jso = NULL;
    assert_true(
      check_import_keys(ffi, &jso, "data/test_key_edge_cases/key-empty-uid.pgp", 1, 1, 0));
    assert_true(
      check_key_status(jso, 0, "new", "none", "753d5b947e9a2b2e01147c1fc972affd358bf887"));
    json_object_put(jso);

    /* key with experimental signature subpackets - must succeed and append uid and signature
     */
    assert_true(check_import_keys(
      ffi, &jso, "data/test_key_edge_cases/key-subpacket-101-110.pgp", 1, 1, 0));
    assert_true(
      check_key_status(jso, 0, "updated", "none", "753d5b947e9a2b2e01147c1fc972affd358bf887"));
    json_object_put(jso);

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "C972AFFD358BF887", &key));
    size_t count = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 2);
    char *uid = NULL;
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uid));
    assert_string_equal(uid, "");
    rnp_buffer_destroy(uid);
    assert_rnp_success(rnp_key_get_uid_at(key, 1, &uid));
    assert_string_equal(uid, "NoUID");
    rnp_buffer_destroy(uid);
    rnp_key_handle_destroy(key);

    /* key with malformed signature - must fail */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/key-malf-sig.pgp"));
    assert_int_equal(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, &results),
                     RNP_ERROR_BAD_FORMAT);
    assert_null(results);
    rnp_input_destroy(input);

    /* revoked key without revocation reason signature subpacket */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-rev-no-reason.pgp"));
    assert_rnp_success(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, &results));
    rnp_input_destroy(input);
    assert_non_null(results);
    rnp_buffer_destroy(results);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_revocation_reason(key, &results));
    assert_string_equal(results, "No reason specified");
    rnp_buffer_destroy(results);
    bool revoked = false;
    assert_rnp_success(rnp_key_is_revoked(key, &revoked));
    assert_true(revoked);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));

    /* revoked subkey without revocation reason signature subpacket */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-sub-rev-no-reason.pgp"));
    assert_rnp_success(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, &results));
    rnp_input_destroy(input);
    assert_non_null(results);
    rnp_buffer_destroy(results);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_int_equal(rnp_key_get_revocation_reason(key, &results), RNP_ERROR_BAD_PARAMETERS);
    revoked = true;
    assert_rnp_success(rnp_key_is_revoked(key, &revoked));
    assert_false(revoked);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &key));
    assert_rnp_success(rnp_key_get_revocation_reason(key, &results));
    assert_string_equal(results, "No reason specified");
    rnp_buffer_destroy(results);
    revoked = false;
    assert_rnp_success(rnp_key_is_revoked(key, &revoked));
    assert_true(revoked);
    rnp_key_handle_destroy(key);

    /* key with two subkeys with same material but different creation time */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-2-subs-same-grip.pgp"));
    assert_rnp_success(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, &results));
    rnp_input_destroy(input);
    assert_non_null(results);
    rnp_buffer_destroy(results);
    count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 3);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    char *keyid = NULL;
    assert_rnp_success(rnp_key_get_keyid(sub, &keyid));
    assert_string_equal(keyid, "DD23CEB7FEBEFF17");
    rnp_buffer_destroy(keyid);
    char *fp = NULL;
    assert_rnp_success(rnp_key_get_primary_fprint(sub, &fp));
    assert_string_equal(fp, "73EDCC9119AFC8E2DBBDCDE50451409669FFDE3C");
    rnp_buffer_destroy(fp);
    rnp_key_handle_destroy(sub);
    assert_rnp_success(rnp_key_get_subkey_at(key, 1, &sub));
    assert_rnp_success(rnp_key_get_keyid(sub, &keyid));
    assert_string_equal(keyid, "C2E7FDCC9CD59FB5");
    rnp_buffer_destroy(keyid);
    assert_rnp_success(rnp_key_get_primary_fprint(sub, &fp));
    assert_string_equal(fp, "73EDCC9119AFC8E2DBBDCDE50451409669FFDE3C");
    rnp_buffer_destroy(fp);
    rnp_key_handle_destroy(sub);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    assert_rnp_success(rnp_key_get_primary_fprint(sub, &fp));
    assert_string_equal(fp, "73EDCC9119AFC8E2DBBDCDE50451409669FFDE3C");
    rnp_buffer_destroy(fp);
    rnp_key_handle_destroy(sub);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "C2E7FDCC9CD59FB5", &sub));
    assert_rnp_success(rnp_key_get_primary_fprint(sub, &fp));
    assert_string_equal(fp, "73EDCC9119AFC8E2DBBDCDE50451409669FFDE3C");
    rnp_buffer_destroy(fp);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* two keys with subkeys with same material but different creation time */
    assert_true(import_pub_keys(ffi, "data/test_key_edge_cases/alice-2-keys-same-grip.pgp"));
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 4);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    assert_rnp_success(rnp_key_get_keyid(sub, &keyid));
    assert_string_equal(keyid, "DD23CEB7FEBEFF17");
    rnp_buffer_destroy(keyid);
    assert_rnp_success(rnp_key_get_primary_fprint(sub, &fp));
    assert_string_equal(fp, "73EDCC9119AFC8E2DBBDCDE50451409669FFDE3C");
    rnp_buffer_destroy(fp);
    rnp_key_handle_destroy(sub);
    assert_rnp_success(rnp_key_get_subkey_at(key, 1, &sub));
    assert_rnp_success(rnp_key_get_keyid(sub, &keyid));
    assert_string_equal(keyid, "C2E7FDCC9CD59FB5");
    rnp_buffer_destroy(keyid);
    assert_rnp_success(rnp_key_get_primary_fprint(sub, &fp));
    assert_string_equal(fp, "73EDCC9119AFC8E2DBBDCDE50451409669FFDE3C");
    rnp_buffer_destroy(fp);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);
    /* subkey should belong to original key */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "467A2DE826ABA0DB", &key));
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 0);
    rnp_key_handle_destroy(key);

    /* key with signing subkey, where primary binding has different from subkey binding hash
     * algorithm */
    assert_true(import_pub_keys(ffi, "data/test_key_edge_cases/key-binding-hash-alg.asc"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "F81A30AA5DCBD01E", &key));
    bool valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_true(key->pub->valid());
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD716516A7249711", &sub));
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    assert_true(sub->pub->valid());
    rnp_key_handle_destroy(sub);

    /* key and subkey both has 0 key expiration with corresponding subpacket */
    assert_true(import_pub_keys(ffi, "data/test_key_edge_cases/key-sub-0-expiry.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "6EFF45F2201AC5F8", &key));
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_true(key->pub->valid());
    uint32_t expiry = 0;
    assert_rnp_success(rnp_key_valid_till(key, &expiry));
    assert_int_equal(expiry, 0xffffffff);
    uint64_t expiry64 = 0;
    assert_rnp_success(rnp_key_valid_till64(key, &expiry64));
    assert_int_equal(expiry64, UINT64_MAX);

    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "74F971795A5DDBC9", &sub));
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    assert_true(sub->pub->valid());
    assert_rnp_success(rnp_key_valid_till(sub, &expiry));
    assert_int_equal(expiry, 0xffffffff);
    assert_rnp_success(rnp_key_valid_till64(sub, &expiry64));
    assert_int_equal(expiry64, UINT64_MAX);
    rnp_key_handle_destroy(sub);

    /* key/subkey with expiration times in unhashed subpackets */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_edge_cases/key-unhashed-subpkts.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7BC6709B15C23A4A", &key));
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_true(key->pub->valid());
    assert_rnp_success(rnp_key_get_expiration(key, &expiry));
    assert_int_equal(expiry, 0);
    assert_rnp_success(rnp_key_valid_till(key, &expiry));
    assert_int_equal(expiry, 0xffffffff);
    assert_rnp_success(rnp_key_valid_till64(key, &expiry64));
    assert_int_equal(expiry64, UINT64_MAX);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1ED63EE56FADC34D", &sub));
    assert_true(sub->pub->valid());
    expiry = 100;
    assert_rnp_success(rnp_key_get_expiration(sub, &expiry));
    assert_int_equal(expiry, 0);
    rnp_key_handle_destroy(sub);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_import_gpg_s2k)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* secret subkeys, exported via gpg --export-secret-subkeys (no primary secret key data) */
    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-1-subs.pgp"));
    assert_rnp_success(rnp_import_keys(
      ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS, NULL));
    rnp_input_destroy(input);
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    bool secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    bool locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    char *type = NULL;
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "GPG-None");
    rnp_buffer_destroy(type);
    assert_rnp_failure(rnp_key_unlock(key, "password"));
    size_t count = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    /* signing secret subkey */
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    char *keyid = NULL;
    assert_rnp_success(rnp_key_get_keyid(sub, &keyid));
    assert_string_equal(keyid, "22F3A217C0E439CB");
    rnp_buffer_destroy(keyid);
    secret = false;
    assert_rnp_success(rnp_key_have_secret(sub, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_unlock(sub, "password"));
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_false(locked);
    rnp_key_handle_destroy(sub);
    /* encrypting secret subkey */
    assert_rnp_success(rnp_key_get_subkey_at(key, 1, &sub));
    assert_rnp_success(rnp_key_get_keyid(sub, &keyid));
    assert_string_equal(keyid, "DD23CEB7FEBEFF17");
    rnp_buffer_destroy(keyid);
    secret = false;
    assert_rnp_success(rnp_key_have_secret(sub, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_unlock(sub, "password"));
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_false(locked);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* save keyrings and reload */
    reload_keyrings(&ffi);

    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "GPG-None");
    rnp_buffer_destroy(type);
    count = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    /* signing secret subkey */
    sub = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    keyid = NULL;
    assert_rnp_success(rnp_key_get_keyid(sub, &keyid));
    assert_string_equal(keyid, "22F3A217C0E439CB");
    rnp_buffer_destroy(keyid);
    secret = false;
    assert_rnp_success(rnp_key_have_secret(sub, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    rnp_key_handle_destroy(sub);
    /* encrypting secret subkey */
    assert_rnp_success(rnp_key_get_subkey_at(key, 1, &sub));
    assert_rnp_success(rnp_key_get_keyid(sub, &keyid));
    assert_string_equal(keyid, "DD23CEB7FEBEFF17");
    rnp_buffer_destroy(keyid);
    secret = false;
    assert_rnp_success(rnp_key_have_secret(sub, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* secret subkeys, and primary key stored on the smartcard by gpg */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-2-card.pgp"));
    assert_rnp_success(rnp_import_keys(
      ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "GPG-Smartcard");
    rnp_buffer_destroy(type);
    assert_rnp_failure(rnp_key_unlock(key, "password"));
    count = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    /* signing secret subkey */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "22F3A217C0E439CB", &sub));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    rnp_key_handle_destroy(sub);
    /* encrypting secret subkey */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(sub, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(sub, &type));
    assert_string_equal(type, "Encrypted-Hashed");
    rnp_buffer_destroy(type);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* save keyrings and reload */
    reload_keyrings(&ffi);

    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    count = 0;
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "GPG-Smartcard");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    rnp_key_handle_destroy(key);

    /* load key with too large gpg_serial_len */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-2-card-len.pgp"));
    assert_rnp_success(rnp_import_keys(
      ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "GPG-Smartcard");
    rnp_buffer_destroy(type);
    assert_rnp_failure(rnp_key_unlock(key, "password"));
    rnp_key_handle_destroy(key);

    /* secret subkeys, and primary key stored with unknown gpg s2k */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-3.pgp"));
    assert_rnp_success(rnp_import_keys(
      ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Unknown");
    rnp_buffer_destroy(type);
    assert_rnp_failure(rnp_key_unlock(key, "password"));
    count = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    rnp_key_handle_destroy(key);

    /* save keyrings and reload */
    reload_keyrings(&ffi);

    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    count = 0;
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Unknown");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    rnp_key_handle_destroy(key);

    /* secret subkeys, and primary key stored with unknown s2k */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-unknown.pgp"));
    assert_rnp_success(rnp_import_keys(
      ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS, NULL));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    locked = false;
    assert_rnp_success(rnp_key_is_locked(key, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Unknown");
    rnp_buffer_destroy(type);
    assert_rnp_failure(rnp_key_unlock(key, "password"));
    count = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    rnp_key_handle_destroy(key);

    /* save keyrings and reload */
    reload_keyrings(&ffi);

    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    count = 0;
    assert_rnp_success(rnp_key_get_protection_type(key, &type));
    assert_string_equal(type, "Unknown");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 2);
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}

static bool
check_key_autocrypt(rnp_output_t       memout,
                    const std::string &keyid,
                    const std::string &subid,
                    const std::string &uid,
                    bool               base64 = false)
{
    rnp_ffi_t ffi = NULL;
    rnp_ffi_create(&ffi, "GPG", "GPG");

    uint8_t *buf = NULL;
    size_t   len = 0;
    if (rnp_output_memory_get_buf(memout, &buf, &len, false) || !buf || !len) {
        return false;
    }
    if (!import_all_keys(ffi, buf, len, base64 ? RNP_LOAD_SAVE_BASE64 : 0)) {
        return false;
    }
    size_t count = 0;
    rnp_get_public_key_count(ffi, &count);
    if (count != 2) {
        return false;
    }
    rnp_get_secret_key_count(ffi, &count);
    if (count != 0) {
        return false;
    }
    rnp_key_handle_t key = NULL;
    if (rnp_locate_key(ffi, "keyid", keyid.c_str(), &key) || !key) {
        return false;
    }
    rnp_key_handle_t sub = NULL;
    if (rnp_locate_key(ffi, "keyid", subid.c_str(), &sub) || !sub) {
        return false;
    }
    if (!key->pub->valid() || !sub->pub->valid()) {
        return false;
    }
    if ((key->pub->sig_count() != 1) || (sub->pub->sig_count() != 1)) {
        return false;
    }
    if (!key->pub->can_sign() || !sub->pub->can_encrypt()) {
        return false;
    }
    if ((key->pub->uid_count() != 1) || (key->pub->get_uid(0).str != uid)) {
        return false;
    }
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);
    rnp_ffi_destroy(ffi);
    return true;
}

TEST_F(rnp_tests, test_ffi_key_export_autocrypt)
{
    rnp_ffi_t ffi = NULL;
    test_ffi_init(&ffi);

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8a05b89fad5aded1", &sub));

    /* edge cases */
    assert_rnp_failure(rnp_key_export_autocrypt(key, NULL, NULL, NULL, 0));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(key, sub, NULL, output, 17));
    assert_rnp_failure(rnp_key_export_autocrypt(NULL, sub, "key0-uid0", output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(key, sub, NULL, output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(key, key, NULL, output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(key, key, "key0-uid0", output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(sub, sub, "key0-uid0", output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(sub, key, "key0-uid0", output, 0));
    assert_int_equal(output->dst.writeb, 0);

    /* export key + uid1 + sub2 */
    assert_rnp_success(rnp_key_export_autocrypt(key, sub, "key0-uid1", output, 0));
    assert_true(
      check_key_autocrypt(output, "7bc6709b15c23a4a", "8a05b89fad5aded1", "key0-uid1"));
    rnp_output_destroy(output);

    /* export key + uid0 + sub1 (fail) */
    rnp_key_handle_destroy(sub);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1d7e8a5393c997a8", &sub));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(key, sub, "key0-uid0", output, 0));
    assert_int_equal(output->dst.writeb, 0);
    rnp_key_handle_destroy(sub);

    /* export key without specifying subkey */
    assert_rnp_success(rnp_key_export_autocrypt(key, NULL, "key0-uid2", output, 0));
    assert_true(
      check_key_autocrypt(output, "7bc6709b15c23a4a", "8a05b89fad5aded1", "key0-uid2"));
    rnp_output_destroy(output);

    /* export base64-encoded key */
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(
      rnp_key_export_autocrypt(key, NULL, "key0-uid2", output, RNP_KEY_EXPORT_BASE64));
    /* Make sure it is base64-encoded */
    const std::string reg = "^[A-Za-z0-9\\+\\/]+={0,2}$";
    uint8_t *         buf = NULL;
    size_t            len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    std::string val((char *) buf, (char *) buf + len);
#ifndef RNP_USE_STD_REGEX
    static regex_t r;
    regmatch_t     matches[1];
    assert_int_equal(regcomp(&r, reg.c_str(), REG_EXTENDED), 0);
    assert_int_equal(regexec(&r, val.c_str(), 1, matches, 0), 0);
#else
    static std::regex re(reg, std::regex_constants::extended | std::regex_constants::icase);
    std::smatch       result;
    assert_true(std::regex_search(val, result, re));
#endif
    /* Fails to load without base64 flag */
    assert_false(import_all_keys(ffi, buf, len));
    /* Now should succeed */
    assert_true(
      check_key_autocrypt(output, "7bc6709b15c23a4a", "8a05b89fad5aded1", "key0-uid2", true));
    rnp_output_destroy(output);

    /* remove first subkey and export again */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1ed63ee56fadc34d", &sub));
    assert_rnp_failure(rnp_key_remove(sub, 0x333));
    assert_rnp_success(rnp_key_remove(sub, RNP_KEY_REMOVE_PUBLIC));
    rnp_key_handle_destroy(sub);
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_key_export_autocrypt(key, NULL, "key0-uid0", output, 0));
    assert_true(
      check_key_autocrypt(output, "7bc6709b15c23a4a", "8a05b89fad5aded1", "key0-uid0"));
    rnp_output_destroy(output);
    rnp_key_handle_destroy(key);

    /* primary key with encrypting capability, make sure subkey is exported */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/encrypting-primary.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "92091b7b76c50017", &key));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_key_export_autocrypt(
      key, NULL, "encrypting primary <encrypting_primary@rnp>", output, 0));
    assert_true(check_key_autocrypt(output,
                                    "92091b7b76c50017",
                                    "c2e243e872c1fe50",
                                    "encrypting primary <encrypting_primary@rnp>"));
    rnp_output_destroy(output);
    rnp_key_handle_destroy(key);

    /* export key with single uid and subkey */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sub-pub.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_key_export_autocrypt(key, NULL, NULL, output, 0));
    assert_true(check_key_autocrypt(
      output, "0451409669ffde3c", "dd23ceb7febeff17", "Alice <alice@rnp>"));
    rnp_output_destroy(output);
    rnp_key_handle_destroy(key);

    /* export key with sign-only subkey: fail */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sign-sub-pub.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "22f3a217c0e439cb", &sub));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(key, sub, NULL, output, 0));
    assert_int_equal(output->dst.writeb, 0);
    assert_rnp_failure(rnp_key_export_autocrypt(key, NULL, NULL, output, 0));
    assert_int_equal(output->dst.writeb, 0);
    rnp_output_destroy(output);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);

    /* export key without subkey: fail */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-pub.asc"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_key_export_autocrypt(key, NULL, NULL, output, 0));
    assert_int_equal(output->dst.writeb, 0);
    rnp_output_destroy(output);
    rnp_key_handle_destroy(key);

    /* export secret key: make sure public is exported */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_all_keys(ffi, "data/test_key_validity/alice-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_key_export_autocrypt(key, NULL, NULL, output, 0));
    assert_true(check_key_autocrypt(
      output, "0451409669ffde3c", "dd23ceb7febeff17", "Alice <alice@rnp>"));
    rnp_output_destroy(output);
    rnp_key_handle_destroy(key);

    /* make sure that only self-certification is exported */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    /* load key alice with 2 self-sigs, one of those is expired */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case9/pubring.gpg"));
    /* add one corrupted alice's signature and one valid from Basil */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case2/pubring.gpg"));

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    assert_int_equal(key->pub->sig_count(), 4);
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_key_export_autocrypt(key, NULL, NULL, output, 0));
    assert_true(check_key_autocrypt(
      output, "0451409669ffde3c", "dd23ceb7febeff17", "Alice <alice@rnp>"));
    rnp_output_destroy(output);
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_keys_import_autocrypt)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_load/ecc-25519-pub.b64"));
    /* no base64 flag */
    assert_rnp_failure(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    rnp_input_destroy(input);
    /* enable base64 flag */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_load/ecc-25519-pub.b64"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_BASE64, NULL));
    rnp_input_destroy(input);
    size_t keycount = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 1);
    /* load other files, with different base64 formatting */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_load/ecc-25519-pub-2.b64"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_BASE64, NULL));
    rnp_input_destroy(input);
    keycount = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 1);

    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_load/ecc-25519-pub-3.b64"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_BASE64, NULL));
    rnp_input_destroy(input);
    keycount = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 1);

    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_load/ecc-25519-pub-4.b64"));
    assert_rnp_failure(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_BASE64, NULL));
    rnp_input_destroy(input);
    keycount = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 0);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_load/ecc-p256k1-pub.b64"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_BASE64, NULL));
    rnp_input_destroy(input);
    keycount = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 2);

    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_load/ecc-p256k1-pub-2.b64"));
    assert_rnp_success(
      rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_BASE64, NULL));
    rnp_input_destroy(input);
    keycount = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keycount));
    assert_int_equal(keycount, 2);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_keys_load_armored_spaces)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    const char *key = R"key(
    -----BEGIN PGP PUBLIC KEY BLOCK-----

mDMEXLO69BYJKwYBBAHaRw8BAQdAWsoBwHOLMrbp7ykSSCD7FYG7tMYT74aLn5wh
Q63nmJC0BmVjZHNhMIiQBBMWCAA4FiEEMuxFQcPhApFLtGbaEJXD7W1DwDsFAlyz
uvQCGwMFCwkIBwIGFQoJCAsCBBYCAwECHgECF4AACgkQEJXD7W1DwDs/cwD+PQt4
GnDUFFW2omo7XJh6AUUC4eUnKQoMWoD3iwYetCwA/1leV7sUdsvs5wvkp+LJVDTW
dbpkwTCmBVbAmazgea0B
=omFJ
-----END PGP PUBLIC KEY BLOCK-----
)key";

    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_memory(&input, (uint8_t *) key, strlen(key), false));
    assert_rnp_success(rnp_load_keys(ffi, "GPG", input, RNP_LOAD_SAVE_PUBLIC_KEYS));
    rnp_input_destroy(input);
    size_t keys = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &keys));
    assert_int_equal(keys, 1);
    rnp_ffi_destroy(ffi);
}

/* Functions below are used to demonstrate how to check whether key has weak MD5/SHA1
 * signatures, and may be reused later in FFI code */
static bool
is_self_signature(const char *keyid, rnp_signature_handle_t sig)
{
    char *signer = NULL;
    rnp_signature_get_keyid(sig, &signer);
    if (!signer) {
        return false;
    }
    bool result = !strcmp(keyid, signer);
    rnp_buffer_destroy(signer);
    return result;
}

static bool
is_weak_signature(rnp_ffi_t ffi, rnp_signature_handle_t sig)
{
    char *   hash = NULL;
    uint32_t creation = 0;
    rnp_signature_get_hash_alg(sig, &hash);
    rnp_signature_get_creation(sig, &creation);
    /* This approach would be more general, however hardcoding MD5/SHA1 may be used as well */
    uint32_t flags = RNP_SECURITY_VERIFY_KEY;
    uint32_t level = 0;
    rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, hash, creation, &flags, NULL, &level);
    bool res = level < RNP_SECURITY_DEFAULT;
    if (res) {
        printf(
          "Detected weak signature with %s hash, created at %zu\n", hash, (size_t) creation);
    }
    rnp_buffer_destroy(hash);
    return res;
}

static const std::string
get_uid_str(rnp_uid_handle_t uid)
{
    uint32_t type = 0;
    rnp_uid_get_type(uid, &type);
    switch (type) {
    case RNP_USER_ID: {
        void * data = NULL;
        size_t len = 0;
        rnp_uid_get_data(uid, &data, &len);
        std::string res((const char *) data, (const char *) data + len);
        rnp_buffer_destroy(data);
        return res;
    }
    case RNP_USER_ATTR:
        return "photo";
    default:
        return "Unknown";
    }
}

static size_t
key_weak_self_signatures_count(rnp_ffi_t ffi, rnp_key_handle_t key)
{
    char *keyid = NULL;
    rnp_key_get_keyid(key, &keyid);
    bool valid = false;
    rnp_key_is_valid(key, &valid);
    printf(
      "Key %s is %s, checking for the weak signatures.\n", keyid, valid ? "valid" : "invalid");
    /* Check direct-key signatures */
    size_t res = 0;
    size_t count = 0;
    rnp_key_get_signature_count(key, &count);
    for (size_t i = 0; i < count; i++) {
        rnp_signature_handle_t sig = NULL;
        rnp_key_get_signature_at(key, i, &sig);
        if (is_self_signature(keyid, sig) && is_weak_signature(ffi, sig)) {
            printf("Key %s has weak direct-key signature at index %zu.\n", keyid, i);
            res++;
        }
        rnp_signature_handle_destroy(sig);
    }
    /* Check certifications */
    size_t uidcount = 0;
    rnp_key_get_uid_count(key, &uidcount);
    for (size_t i = 0; i < uidcount; i++) {
        rnp_uid_handle_t uid = NULL;
        rnp_key_get_uid_handle_at(key, i, &uid);
        count = 0;
        rnp_uid_get_signature_count(uid, &count);
        for (size_t j = 0; j < count; j++) {
            rnp_signature_handle_t sig = NULL;
            rnp_uid_get_signature_at(uid, j, &sig);
            if (is_self_signature(keyid, sig) && is_weak_signature(ffi, sig)) {
                auto uidstr = get_uid_str(uid);
                printf("Uid %s of the key %s has weak self-certification at index %zu.\n",
                       uidstr.c_str(),
                       keyid,
                       j);
                res++;
            }
            rnp_signature_handle_destroy(sig);
        }
        rnp_uid_handle_destroy(uid);
    }
    /* Check subkeys */
    size_t subcount = 0;
    rnp_key_get_subkey_count(key, &subcount);
    for (size_t i = 0; i < subcount; i++) {
        rnp_key_handle_t subkey = NULL;
        rnp_key_get_subkey_at(key, i, &subkey);
        count = 0;
        rnp_key_get_signature_count(subkey, &count);
        for (size_t j = 0; j < count; j++) {
            rnp_signature_handle_t sig = NULL;
            rnp_key_get_signature_at(subkey, j, &sig);
            if (is_self_signature(keyid, sig) && is_weak_signature(ffi, sig)) {
                char *subid = NULL;
                rnp_key_get_keyid(subkey, &subid);
                printf("Subkey %s of the key %s has weak binding signature at index %zu.\n",
                       subid,
                       keyid,
                       j);
                res++;
                rnp_buffer_destroy(subid);
            }
            rnp_signature_handle_destroy(sig);
        }
        rnp_key_handle_destroy(subkey);
    }
    rnp_buffer_destroy(keyid);
    return res;
}

TEST_F(rnp_tests, test_ffi_sha1_self_signatures)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* This key has SHA1 self signature, made before the cut-off date */
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/rsa-rsa-pub.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2fb9179118898e8b", &key));
    /* Check key validity */
    bool valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    size_t count = 0;
    /* Check uid validity */
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 1);
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    /* Check subkey validity */
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 1);
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    /* Check weak signature count */
    assert_int_equal(key_weak_self_signatures_count(ffi, key), 0);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));

    /* Check the key which has SHA1 self signature, made after the cut-off date */
    assert_rnp_failure(rnp_set_timestamp(NULL, SHA1_KEY_FROM + 10));
    assert_rnp_success(rnp_set_timestamp(ffi, SHA1_KEY_FROM + 10));
    assert_true(import_pub_keys(ffi, "data/test_forged_keys/eddsa-2024-pub.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "980e3741f632212c", &key));
    /* Check key validity */
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_false(valid);
    /* Check uid validity */
    assert_rnp_success(rnp_key_get_uid_count(key, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    rnp_uid_handle_destroy(uid);
    /* Check subkey validity */
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_false(valid);
    /* Check weak signature count */
    assert_int_equal(key_weak_self_signatures_count(ffi, key), 2);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    /* Now allow the SHA1 hash */
    assert_rnp_success(rnp_add_security_rule(ffi,
                                             RNP_FEATURE_HASH_ALG,
                                             "SHA1",
                                             RNP_SECURITY_OVERRIDE,
                                             SHA1_KEY_FROM + 1,
                                             RNP_SECURITY_DEFAULT));

    assert_true(import_pub_keys(ffi, "data/test_forged_keys/eddsa-2024-pub.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "980e3741f632212c", &key));
    /* Check key validity */
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    /* Check uid validity */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    /* Check subkey validity */
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    /* Check weak signature count */
    assert_int_equal(key_weak_self_signatures_count(ffi, key), 0);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* Check the key which has MD5 self signature, made after the cut-off date */
    assert_true(import_pub_keys(ffi, "data/test_forged_keys/eddsa-2012-md5-pub.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8801eafbd906bd21", &key));
    /* Check key validity */
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_false(valid);
    /* Check uid validity */
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    rnp_uid_handle_destroy(uid);
    /* Check subkey validity */
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_false(valid);
    /* Check weak signature count */
    assert_int_equal(key_weak_self_signatures_count(ffi, key), 2);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_reprotect_keys)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* Cast5-encrypted keys */
    assert_true(
      load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring-cast5.gpg"));

    rnp_identifier_iterator_t it = NULL;
    assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "fingerprint"));
    assert_non_null(it);
    const char *ident = NULL;
    do {
        ident = NULL;
        assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
        if (!ident) {
            break;
        }
        rnp_key_handle_t key = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "fingerprint", ident, &key));
        if (cast5_enabled()) {
            assert_rnp_success(rnp_key_unprotect(key, "password"));
            assert_rnp_success(rnp_key_protect(key, "password", "AES256", NULL, NULL, 65536));
        } else {
            assert_rnp_failure(rnp_key_unprotect(key, "password"));
        }
        rnp_key_handle_destroy(key);
    } while (1);
    assert_rnp_success(rnp_identifier_iterator_destroy(it));
    /* AES-encrypted keys */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(
      load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring.gpg"));
    assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "fingerprint"));
    assert_non_null(it);
    do {
        ident = NULL;
        assert_rnp_success(rnp_identifier_iterator_next(it, &ident));
        if (!ident) {
            break;
        }
        rnp_key_handle_t key = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "fingerprint", ident, &key));
        assert_rnp_success(rnp_key_unprotect(key, "password"));
        if (cast5_enabled()) {
            assert_rnp_success(rnp_key_protect(key, "password", "CAST5", NULL, NULL, 65536));
        } else {
            assert_rnp_success(rnp_key_protect(key, "password", "AES128", NULL, NULL, 65536));
        }
        rnp_key_handle_destroy(key);
    } while (1);
    assert_rnp_success(rnp_identifier_iterator_destroy(it));
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_v5_keys)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* v5 rsa-rsa public key */
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/v5-rsa-pub.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(
      rnp_locate_key(ffi,
                     "fingerprint",
                     "b856a4197113d431927b925248f026615f9f390b26bc1676e81f072c70f539e9",
                     &key));
    assert_true(check_key_valid(key, true));
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_sub_valid(key, 0, true));
    assert_true(check_key_grip(key, "442238389AFF3D83492606F0139655330EECA70E"));
    rnp_key_handle_destroy(key);
    /* Locate subkey via keyid */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2d400055b0345c33", &key));
    assert_true(check_key_valid(key, true));
    assert_true(
      check_key_fp(key, "2D400055B0345C33363F03A72F4D2363C18298ED005780BFB2C4351FEE15446C"));
    assert_true(check_key_grip(key, "E68FD5C5250C21D4D4646226C9A048729B2DDC21"));
    rnp_key_handle_destroy(key);

    /* add v5 rsa-rsa secret key */
    assert_true(import_sec_keys(ffi, "data/test_stream_key_load/v5-rsa-sec.asc"));
    assert_true(check_has_key(ffi, "b856a4197113d431", true));
    assert_true(check_has_key(ffi, "2d400055b0345c33", true));

    /* v5 dsa-eg public key */
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/v5-dsa-eg-pub.asc"));
    key = NULL;
    assert_rnp_success(
      rnp_locate_key(ffi,
                     "fingerprint",
                     "3069f583308ac3e4a5517a07c487756da7f7456cee5e2bdf00411b7bb00dee82",
                     &key));
    assert_true(check_key_valid(key, true));
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_sub_valid(key, 0, true));
    assert_true(check_key_grip(key, "48ABC799737F65015C143E28E80BF91018F101D5"));
    rnp_key_handle_destroy(key);
    /* subkey */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8514df0cab25f0d3", &key));
    assert_true(check_key_valid(key, true));
    assert_true(
      check_key_fp(key, "8514DF0CAB25F0D34768F09850BE1CB6EF007630F8D02E5E2DEAD1C83C30412D"));
    assert_true(check_key_grip(key, "B14996A317484FC50DAB2B01F49B803E29DC687A"));
    rnp_key_handle_destroy(key);
    /* secret key */
    assert_true(import_sec_keys(ffi, "data/test_stream_key_load/v5-dsa-eg-sec.asc"));
    assert_true(check_has_key(ffi, "3069f583308ac3e4", true));
    assert_true(check_has_key(ffi, "8514df0cab25f0d3", true));

    /* v5 ecc 25519 key */
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/v5-ecc-25519-pub.asc"));
    key = NULL;
    assert_rnp_success(
      rnp_locate_key(ffi,
                     "fingerprint",
                     "817f60336bb9d133b59f1b91fdebe36796c1b2f47907bab49a7eb981dc719dc0",
                     &key));
    assert_true(check_key_valid(key, true));
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_sub_valid(key, 0, true));
    assert_true(check_key_grip(key, "AC3EA2D975FF76029DFE1E9AB01F5DB36CF8B912"));
    rnp_key_handle_destroy(key);
    /* subkey */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "08b67c2205cfd75b", &key));
    assert_true(check_key_valid(key, true));
    assert_true(
      check_key_fp(key, "08B67C2205CFD75BF6E545BBFF075AAAAB1A0A37E75E05699D892B797FB02493"));
    assert_true(check_key_grip(key, "49F25BE1255F2A726B79DF52D5EC87160C47A11D"));
    rnp_key_handle_destroy(key);
    /* secret key */
    assert_true(import_sec_keys(ffi, "data/test_stream_key_load/v5-ecc-25519-sec.asc"));
    assert_true(check_has_key(ffi, "817f60336bb9d133", true));
    assert_true(check_has_key(ffi, "08b67c2205cfd75b", true));

    /* v5 ecc 448 key : not supported yet */
    assert_false(import_pub_keys(ffi, "data/test_stream_key_load/v5-ecc-448-pub.asc"));
    assert_false(import_sec_keys(ffi, "data/test_stream_key_load/v5-ecc-448-sec.asc"));

    /* v5 ecc p256 key */
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/v5-ecc-p256-pub.asc"));
    key = NULL;
    assert_rnp_success(
      rnp_locate_key(ffi,
                     "fingerprint",
                     "de96db9d6198a7a0183e29e56e48d548ca914a999fe99fbad93d077ebe61a1ef",
                     &key));
    assert_true(check_key_valid(key, true));
    assert_true(check_uid_valid(key, 0, true));
    assert_true(check_sub_valid(key, 0, true));
    assert_true(check_key_grip(key, "C4AC4AE27A21DA9B760573133E07E443C562C0E6"));
    rnp_key_handle_destroy(key);
    /* subkey */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "da25a0907380d168", &key));
    assert_true(check_key_valid(key, true));
    assert_true(
      check_key_fp(key, "DA25A0907380D16850850C89B01D9466E79A714990595A55AA477B9CE60E970F"));
    assert_true(check_key_grip(key, "A8B7B80C256BB50C997FD38902C434C281946A43"));
    rnp_key_handle_destroy(key);
    /* secret key */
    assert_true(import_sec_keys(ffi, "data/test_stream_key_load/v5-ecc-p256-sec.asc"));
    assert_true(check_has_key(ffi, "de96db9d6198a7a0", true));
    assert_true(check_has_key(ffi, "da25a0907380d168", true));

    reload_keyrings(&ffi);
    /* v5 rsa */
    assert_true(check_has_key(ffi, "b856a4197113d431", true));
    assert_true(check_has_key(ffi, "2d400055b0345c33", true));
    /* v5 dsa-eg */
    assert_true(check_has_key(ffi, "3069f583308ac3e4", true));
    assert_true(check_has_key(ffi, "8514df0cab25f0d3", true));
    /* v5 ecc 25519 */
    assert_true(check_has_key(ffi, "817f60336bb9d133", true));
    assert_true(check_has_key(ffi, "08b67c2205cfd75b", true));
    /* v5 p256 */
    assert_true(check_has_key(ffi, "de96db9d6198a7a0", true));
    assert_true(check_has_key(ffi, "da25a0907380d168", true));

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_v5_keys_g23)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    /* New format which is not supported yet */
    assert_false(load_keys_kbx_g10(ffi,
                                   "data/test_stream_key_load/g23-v5/pubring.kbx",
                                   "data/test_stream_key_load/g23-v5/private-keys-v1.d"));

    /* v5 rsa */
    assert_false(check_has_key(ffi, "b856a4197113d431", true));
    assert_false(check_has_key(ffi, "2d400055b0345c33", true));
    /* v5 dsa-eg */
    assert_false(check_has_key(ffi, "3069f583308ac3e4", true));
    assert_false(check_has_key(ffi, "8514df0cab25f0d3", true));
    /* v5 ecc 25519 */
    assert_false(check_has_key(ffi, "817f60336bb9d133", true));
    assert_false(check_has_key(ffi, "08b67c2205cfd75b", true));
    /* v5 p256 */
    assert_false(check_has_key(ffi, "de96db9d6198a7a0", true));
    assert_false(check_has_key(ffi, "da25a0907380d168", true));

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_v5_sec_keys)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* v5 rsa-rsa secret key */
    assert_true(import_sec_keys(ffi, "data/test_stream_key_load/v5-rsa-sec.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "b856a4197113d431", &key));
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_success(rnp_key_lock(key));
    rnp_key_handle_destroy(key);
    /* v5 rsa secret subkey */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2d400055b0345c33", &key));
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_success(rnp_key_lock(key));
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_designated_revokers)
{
    auto path_for = [](const std::string &file) {
        return "data/test_stream_key_load/" + file;
    };
    rnp_ffi_t ffi = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* Load key, revoked by designated revocation key */
    assert_true(load_keys_gpg(ffi, path_for("ecc-p256-desigrevoked-25519-pub.asc")));
    /* Check whether it is revoked - not yet as there is no revoker's key */
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_non_null(key);
    assert_true(check_key_valid(key, true));
    assert_true(check_key_revoked(key, false));
    rnp_signature_handle_t sig = NULL;
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &sig));
    char *sigtype = NULL;
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "key revocation");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_KEY_NOT_FOUND);
    /* Check for empty designated revoker */
    char *revoker = NULL;
    assert_rnp_failure(rnp_signature_get_revoker(NULL, &revoker));
    assert_rnp_failure(rnp_signature_get_revoker(sig, NULL));
    assert_rnp_success(rnp_signature_get_revoker(sig, &revoker));
    assert_int_equal(strcmp(revoker, ""), 0);
    rnp_buffer_destroy(revoker);
    rnp_signature_handle_destroy(sig);
    /* Now not empty */
    assert_rnp_success(rnp_key_get_signature_at(key, 1, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "direct");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_get_revoker(sig, &revoker));
    assert_int_equal(strcmp(revoker, "21FC68274AAE3B5DE39A4277CC786278981B0728"), 0);
    rnp_buffer_destroy(revoker);
    rnp_signature_handle_destroy(sig);
    rnp_key_handle_destroy(key);
    /* Load revoker's key and recheck */
    assert_true(load_keys_gpg(ffi, path_for("ecc-25519-pub.asc")));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_non_null(key);
    assert_true(check_key_valid(key, false));
    assert_true(check_key_revoked(key, true));
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "key revocation");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_SUCCESS);
    rnp_signature_handle_destroy(sig);
    rnp_key_handle_destroy(key);
    /* Load first revoker's key and then revoked one */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, path_for("ecc-25519-pub.asc")));
    assert_true(import_pub_keys(ffi, path_for("ecc-p256-desigrevoked-25519-pub.asc")));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_true(check_key_valid(key, false));
    assert_true(check_key_revoked(key, true));
    rnp_key_handle_destroy(key);
    /* Load key with revocation from non-designated revoker */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, path_for("ecc-25519-pub.asc")));
    assert_true(import_pub_keys(ffi, path_for("ecc-p384-pub.asc")));
    assert_true(import_pub_keys(ffi, path_for("ecc-p256-desig-wrong-revoker.pgp")));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_true(check_key_valid(key, true));
    assert_true(check_key_revoked(key, false));
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &sig));
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_SUCCESS);
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "direct");
    rnp_buffer_destroy(sigtype);
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_key_get_signature_at(key, 1, &sig));
    /* While signature is technically valid, it is not applicable to the key */
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_SUCCESS);
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "key revocation");
    rnp_buffer_destroy(sigtype);
    rnp_signature_handle_destroy(sig);
    rnp_key_handle_destroy(key);
    /* Add key to force refresh and check back to make sure revocation status did not change */
    assert_true(import_pub_keys(ffi, path_for("ecc-p521-pub.asc")));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_true(check_key_valid(key, true));
    assert_true(check_key_revoked(key, false));
    rnp_key_handle_destroy(key);
    /* Key with 2 designated revokers and 2 revocations */
    assert_true(load_keys_gpg(ffi, path_for("ecc-p256-desigrevoked-2-revs.pgp")));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    /* Check designated revokers */
    size_t count = 0;
    assert_rnp_failure(rnp_key_get_revoker_count(NULL, &count));
    assert_rnp_failure(rnp_key_get_revoker_count(key, NULL));
    assert_rnp_success(rnp_key_get_revoker_count(key, &count));
    assert_int_equal(count, 2);
    assert_rnp_failure(rnp_key_get_revoker_at(NULL, 0, &revoker));
    assert_rnp_failure(rnp_key_get_revoker_at(key, 0, NULL));
    assert_rnp_failure(rnp_key_get_revoker_at(key, 2, &revoker));
    assert_rnp_failure(rnp_key_get_revoker_at(key, (size_t) -1, &revoker));
    assert_rnp_success(rnp_key_get_revoker_at(key, 0, &revoker));
    assert_string_equal(revoker, "21FC68274AAE3B5DE39A4277CC786278981B0728");
    rnp_buffer_destroy(revoker);
    assert_rnp_success(rnp_key_get_revoker_at(key, 1, &revoker));
    assert_string_equal(revoker, "AB25CBA042DD924C3ACC3ED3242A3AA5EA85F44A");
    rnp_buffer_destroy(revoker);
    /* Check key validity */
    assert_true(check_key_valid(key, true));
    /* key is revoked since designated revocation is already in the keyring and was checked
     * with ecc-p384 key */
    assert_true(check_key_revoked(key, true));
    rnp_key_handle_destroy(key);
    assert_true(load_keys_gpg(ffi, path_for("ecc-p384-pub.asc")));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_true(check_key_valid(key, false));
    assert_true(check_key_revoked(key, true));
    char *rev_reason = NULL;
    assert_rnp_success(rnp_key_get_revocation_reason(key, &rev_reason));
    assert_string_equal(rev_reason, "ecc-p384 revocation for ecc-p256");
    rnp_buffer_destroy(rev_reason);
    rnp_key_handle_destroy(key);
    /* Now try another revoker */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(load_keys_gpg(ffi, path_for("ecc-p256-desigrevoked-2-revs.pgp")));
    assert_true(load_keys_gpg(ffi, path_for("ecc-25519-pub.asc")));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_true(check_key_valid(key, false));
    assert_true(check_key_revoked(key, true));
    assert_rnp_success(rnp_key_get_revocation_reason(key, &rev_reason));
    assert_string_equal(rev_reason, "ecc-25519 revocation for ecc-p256");
    rnp_buffer_destroy(rev_reason);
    rnp_key_handle_destroy(key);
    /* Case where revocation signatures goes before the direct-key with desig revoker */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(load_keys_gpg(ffi, path_for("ecc-p256-desigrevoked-sigorder.pgp")));
    assert_true(import_pub_keys(ffi, path_for("ecc-p384-pub.asc")));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_true(check_key_valid(key, false));
    assert_true(check_key_revoked(key, true));
    assert_rnp_success(rnp_key_get_revocation_reason(key, &rev_reason));
    assert_string_equal(rev_reason, "ecc-p384 revocation for ecc-p256");
    rnp_buffer_destroy(rev_reason);
    rnp_key_handle_destroy(key);

    /* Cleanup */
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_armored_keys_extra_line)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* Key with extra line after the checksum */
    assert_true(
      import_pub_keys(ffi, "data/test_stream_key_load/ecc-25519-pub-extra-line.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0xcc786278981b0728", &key));
    assert_true(check_key_valid(key, true));
    assert_true(check_uid_valid(key, 0, true));
    rnp_key_handle_destroy(key);

    /* Key with extra lines with spaces after the checksum */
    assert_true(
      import_pub_keys(ffi, "data/test_stream_key_load/ecc-25519-pub-extra-line-2.asc"));
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0xcc786278981b0728", &key));
    assert_true(check_key_valid(key, true));
    assert_true(check_uid_valid(key, 0, true));
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}
