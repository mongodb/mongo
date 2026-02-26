/*
 * Copyright (c) 2017-2023 [Ribose Inc](https://www.ribose.com).
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

#include <fstream>
#include <vector>
#include <string>
#include <set>
#include <utility>
#include <cstdint>

#include <rnp/rnp.h>
#include "rnp_tests.h"
#include "support.h"
#include "librepgp/stream-common.h"
#include "librepgp/stream-packet.h"
#include "librepgp/stream-sig.h"
#include <json.h>
#include "file-utils.h"
#include "str-utils.h"
#include <librepgp/stream-ctx.h>
#include "key.hpp"
#include "ffi-priv-types.h"

TEST_F(rnp_tests, test_ffi_homedir)
{
    rnp_ffi_t ffi = NULL;
    char *    pub_format = NULL;
    char *    pub_path = NULL;
    char *    sec_format = NULL;
    char *    sec_path = NULL;

    // get the default homedir (not a very thorough test)
    {
        assert_rnp_failure(rnp_get_default_homedir(NULL));
        char *homedir = NULL;
        assert_rnp_success(rnp_get_default_homedir(&homedir));
        assert_non_null(homedir);
        rnp_buffer_destroy(homedir);
    }

    // check NULL params
    assert_rnp_failure(
      rnp_detect_homedir_info(NULL, &pub_format, &pub_path, &sec_format, &sec_path));
    assert_rnp_failure(
      rnp_detect_homedir_info("data/keyrings/1", NULL, &pub_path, &sec_format, &sec_path));
    assert_rnp_failure(
      rnp_detect_homedir_info("data/keyrings/1", &pub_format, NULL, &sec_format, &sec_path));
    assert_rnp_failure(
      rnp_detect_homedir_info("data/keyrings/1", &pub_format, &pub_path, NULL, &sec_path));
    assert_rnp_failure(
      rnp_detect_homedir_info("data/keyrings/1", &pub_format, &pub_path, &sec_format, NULL));
    // detect the formats+paths
    assert_rnp_success(rnp_detect_homedir_info(
      "data/keyrings/1", &pub_format, &pub_path, &sec_format, &sec_path));
    // check formats
    assert_string_equal(pub_format, "GPG");
    assert_string_equal(sec_format, "GPG");
    // check paths
    assert_string_equal(pub_path, "data/keyrings/1/pubring.gpg");
    assert_string_equal(sec_path, "data/keyrings/1/secring.gpg");
    rnp_buffer_destroy(pub_format);
    rnp_buffer_destroy(pub_path);
    rnp_buffer_destroy(sec_format);
    rnp_buffer_destroy(sec_path);
// detect windows-style slashes
#ifdef _WIN32
    assert_rnp_success(rnp_detect_homedir_info(
      "data\\keyrings\\1", &pub_format, &pub_path, &sec_format, &sec_path));
    // check formats
    assert_string_equal(pub_format, "GPG");
    assert_string_equal(sec_format, "GPG");
    // check paths
    assert_string_equal(pub_path, "data\\keyrings\\1\\pubring.gpg");
    assert_string_equal(sec_path, "data\\keyrings\\1\\secring.gpg");
    rnp_buffer_destroy(pub_format);
    rnp_buffer_destroy(pub_path);
    rnp_buffer_destroy(sec_format);
    rnp_buffer_destroy(sec_path);
#endif
    // detect with the trailing slash
    assert_rnp_success(rnp_detect_homedir_info(
      "data/keyrings/1/", &pub_format, &pub_path, &sec_format, &sec_path));
    // check formats
    assert_string_equal(pub_format, "GPG");
    assert_string_equal(sec_format, "GPG");
    // check paths
    assert_string_equal(pub_path, "data/keyrings/1/pubring.gpg");
    assert_string_equal(sec_path, "data/keyrings/1/secring.gpg");
    // setup FFI with wrong parameters
    assert_rnp_failure(rnp_ffi_create(NULL, "GPG", "GPG"));
    assert_rnp_failure(rnp_ffi_create(&ffi, "GPG", NULL));
    assert_rnp_failure(rnp_ffi_create(&ffi, NULL, "GPG"));
    assert_rnp_failure(rnp_ffi_create(&ffi, "ZZG", "GPG"));
    assert_rnp_failure(rnp_ffi_create(&ffi, "GPG", "ZZG"));
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
    // check key counts
    size_t count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(7, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(7, count);
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;

    // detect the formats+paths
    assert_rnp_success(rnp_detect_homedir_info(
      "data/keyrings/3", &pub_format, &pub_path, &sec_format, &sec_path));
    // check formats
    assert_string_equal(pub_format, "KBX");
    assert_string_equal(sec_format, "G10");
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, pub_format, sec_format));
    // load our keyrings
    assert_true(load_keys_kbx_g10(ffi, pub_path, sec_path));
    // free formats+paths
    rnp_buffer_destroy(pub_format);
    pub_format = NULL;
    rnp_buffer_destroy(pub_path);
    pub_path = NULL;
    rnp_buffer_destroy(sec_format);
    sec_format = NULL;
    rnp_buffer_destroy(sec_path);
    sec_path = NULL;
    // check key counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(2, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(2, count);
    // check grip (1)
    rnp_key_handle_t key = NULL;
    assert_int_equal(
      RNP_SUCCESS,
      rnp_locate_key(ffi, "grip", "63E59092E4B1AE9F8E675B2F98AA2B8BD9F4EA59", &key));
    assert_non_null(key);
    char *grip = NULL;
    assert_rnp_success(rnp_key_get_grip(key, &grip));
    assert_non_null(grip);
    assert_string_equal(grip, "63E59092E4B1AE9F8E675B2F98AA2B8BD9F4EA59");
    rnp_buffer_destroy(grip);
    grip = NULL;
    rnp_key_handle_destroy(key);
    key = NULL;
    // check grip (2)
    assert_int_equal(
      RNP_SUCCESS,
      rnp_locate_key(ffi, "grip", "7EAB41A2F46257C36F2892696F5A2F0432499AD3", &key));
    assert_non_null(key);
    grip = NULL;
    assert_rnp_success(rnp_key_get_grip(key, &grip));
    assert_non_null(grip);
    assert_string_equal(grip, "7EAB41A2F46257C36F2892696F5A2F0432499AD3");
    rnp_buffer_destroy(grip);
    grip = NULL;
    assert_rnp_success(rnp_key_handle_destroy(key));
    key = NULL;
    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_detect_key_format)
{
    char *format = NULL;

    // Wrong parameters
    auto data = file_to_vec("data/keyrings/1/pubring.gpg");
    assert_rnp_failure(rnp_detect_key_format(NULL, data.size(), &format));
    assert_rnp_failure(rnp_detect_key_format(data.data(), 0, &format));
    assert_rnp_failure(rnp_detect_key_format(data.data(), data.size(), NULL));
    free(format);

    // GPG
    format = NULL;
    data = file_to_vec("data/keyrings/1/pubring.gpg");
    assert_rnp_success(rnp_detect_key_format(data.data(), data.size(), &format));
    assert_string_equal(format, "GPG");
    free(format);
    format = NULL;

    // GPG
    data = file_to_vec("data/keyrings/1/secring.gpg");
    assert_rnp_success(rnp_detect_key_format(data.data(), data.size(), &format));
    assert_string_equal(format, "GPG");
    free(format);
    format = NULL;

    // GPG (armored)
    data = file_to_vec("data/keyrings/4/rsav3-p.asc");
    assert_rnp_success(rnp_detect_key_format(data.data(), data.size(), &format));
    assert_string_equal(format, "GPG");
    free(format);
    format = NULL;

    // KBX
    data = file_to_vec("data/keyrings/3/pubring.kbx");
    assert_rnp_success(rnp_detect_key_format(data.data(), data.size(), &format));
    assert_string_equal(format, "KBX");
    free(format);
    format = NULL;

    // G10
    data = file_to_vec(
      "data/keyrings/3/private-keys-v1.d/63E59092E4B1AE9F8E675B2F98AA2B8BD9F4EA59.key");
    assert_rnp_success(rnp_detect_key_format(data.data(), data.size(), &format));
    assert_string_equal(format, "G10");
    free(format);
    format = NULL;

    // invalid
    assert_rnp_success(rnp_detect_key_format((uint8_t *) "ABC", 3, &format));
    assert_null(format);
}

TEST_F(rnp_tests, test_ffi_load_keys)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;
    size_t      count;

    /* load public keys from pubring */
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load pubring
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));
    // again
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));
    // check counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(7, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(0, count);
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;

    /* load public keys from secring */
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/secring.gpg"));
    // check counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(7, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(0, count);
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;

    /* load secret keys from secring */
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load secring
    assert_true(load_keys_gpg(ffi, "", "data/keyrings/1/secring.gpg"));
    // again
    assert_true(load_keys_gpg(ffi, "", "data/keyrings/1/secring.gpg"));
    // check counts
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(7, count);
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(0, count);
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;

    /* load secret keys from pubring */
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load pubring
    assert_true(load_keys_gpg(ffi, "", "data/keyrings/1/pubring.gpg"));
    // check counts
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(0, count);
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(0, count);
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_ffi_destroy(ffi);
    ffi = NULL;

    /* concatenate the pubring and secrings into a single buffer */
    auto pub_buf = file_to_vec("data/keyrings/1/pubring.gpg");
    auto sec_buf = file_to_vec("data/keyrings/1/secring.gpg");
    auto buf = pub_buf;
    buf.reserve(buf.size() + sec_buf.size());
    buf.insert(buf.end(), sec_buf.begin(), sec_buf.end());

    /* load secret keys from pubring */
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load
    assert_rnp_success(rnp_input_from_memory(&input, buf.data(), buf.size(), true));
    assert_non_null(input);
    // try wrong parameters
    assert_rnp_failure(rnp_load_keys(NULL, "GPG", input, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_failure(rnp_load_keys(ffi, NULL, input, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_failure(rnp_load_keys(ffi, "GPG", NULL, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_failure(rnp_load_keys(ffi, "WRONG", input, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_failure(rnp_load_keys(ffi, "WRONG", input, 0));
    assert_rnp_success(rnp_load_keys(ffi, "GPG", input, RNP_LOAD_SAVE_SECRET_KEYS));
    rnp_input_destroy(input);
    input = NULL;
    // again
    assert_rnp_success(rnp_input_from_memory(&input, buf.data(), buf.size(), true));
    assert_non_null(input);
    assert_rnp_success(rnp_load_keys(ffi, "GPG", input, RNP_LOAD_SAVE_SECRET_KEYS));
    rnp_input_destroy(input);
    input = NULL;
    // check counts
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(7, count);
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_ffi_destroy(ffi);
    ffi = NULL;
}

TEST_F(rnp_tests, test_ffi_clear_keys)
{
    rnp_ffi_t ffi = NULL;
    size_t    pub_count;
    size_t    sec_count;

    // setup FFI
    test_ffi_init(&ffi);
    // check counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &pub_count));
    assert_int_equal(7, pub_count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &sec_count));
    assert_int_equal(7, sec_count);
    // clear public keys
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_rnp_success(rnp_get_public_key_count(ffi, &pub_count));
    assert_int_equal(pub_count, 0);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &sec_count));
    assert_int_equal(sec_count, 7);
    // clear secret keys
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(rnp_get_public_key_count(ffi, &pub_count));
    assert_int_equal(pub_count, 0);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &sec_count));
    assert_int_equal(sec_count, 0);
    // load public and clear secret keys
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(rnp_get_public_key_count(ffi, &pub_count));
    assert_int_equal(pub_count, 7);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &sec_count));
    assert_int_equal(sec_count, 0);
    // load secret keys and clear all
    assert_true(load_keys_gpg(ffi, "", "data/keyrings/1/secring.gpg"));
    assert_rnp_success(rnp_get_public_key_count(ffi, &pub_count));
    assert_int_equal(7, pub_count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &sec_count));
    assert_int_equal(7, sec_count);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(rnp_get_public_key_count(ffi, &pub_count));
    assert_int_equal(pub_count, 0);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &sec_count));
    assert_int_equal(sec_count, 0);
    // attempt to clear NULL ffi
    assert_rnp_failure(rnp_unload_keys(NULL, RNP_KEY_UNLOAD_SECRET));
    // attempt to pass wrong flags
    assert_rnp_failure(rnp_unload_keys(ffi, 255));
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;
}

TEST_F(rnp_tests, test_ffi_save_keys)
{
    rnp_ffi_t ffi = NULL;
    // setup FFI
    test_ffi_init(&ffi);
    char *temp_dir = make_temp_dir();
    // save pubring
    auto pub_path = rnp::path::append(temp_dir, "pubring.gpg");
    assert_false(rnp::path::exists(pub_path));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_path(&output, pub_path.c_str()));
    assert_rnp_failure(rnp_save_keys(NULL, "GPG", output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_failure(rnp_save_keys(ffi, NULL, output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_failure(rnp_save_keys(ffi, "GPG", NULL, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_failure(rnp_save_keys(ffi, "WRONG", output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_failure(rnp_save_keys(ffi, "GPG", output, 0));
    assert_rnp_failure(rnp_save_keys(ffi, "GPG", output, 0x77));
    assert_rnp_success(rnp_save_keys(ffi, "GPG", output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_true(rnp::path::exists(pub_path));
    // save secring
    auto sec_path = rnp::path::append(temp_dir, "secring.gpg");
    assert_false(rnp::path::exists(sec_path));
    assert_rnp_success(rnp_output_to_path(&output, sec_path.c_str()));
    assert_rnp_success(rnp_save_keys(ffi, "GPG", output, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_true(rnp::path::exists(sec_path));
    // save pubring && secring
    auto both_path = rnp::path::append(temp_dir, "bothring.gpg");
    assert_false(rnp::path::exists(both_path));
    assert_rnp_success(rnp_output_to_path(&output, both_path.c_str()));
    assert_int_equal(
      RNP_SUCCESS,
      rnp_save_keys(
        ffi, "GPG", output, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_true(rnp::path::exists(both_path));
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;
    // start over (read from the saved locations)
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load pubring & secring
    assert_true(load_keys_gpg(ffi, pub_path, sec_path));
    // check the counts
    size_t count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(7, count);
    count = 0;
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(7, count);
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;
    // load both keyrings from the single file
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load pubring
    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_path(&input, both_path.c_str()));
    assert_non_null(input);
    assert_int_equal(
      RNP_SUCCESS,
      rnp_load_keys(ffi, "GPG", input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    rnp_input_destroy(input);
    input = NULL;
    // check the counts. We should get both secret and public keys, since public keys are
    // extracted from the secret ones.
    count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(7, count);
    count = 0;
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(7, count);
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    // load pubring & secring
    assert_true(load_keys_kbx_g10(
      ffi, "data/keyrings/3/pubring.kbx", "data/keyrings/3/private-keys-v1.d"));
    // save pubring
    pub_path = rnp::path::append(temp_dir, "pubring.kbx");
    assert_rnp_success(rnp_output_to_path(&output, pub_path.c_str()));
    assert_rnp_success(rnp_save_keys(ffi, "KBX", output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_true(rnp::path::exists(pub_path));
    // save secring to file - will fail for G10
    sec_path = rnp::path::append(temp_dir, "secring.file");
    assert_rnp_success(rnp_output_to_path(&output, sec_path.c_str()));
    assert_rnp_failure(rnp_save_keys(ffi, "G10", output, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    // save secring
    sec_path = rnp::path::append(temp_dir, "private-keys-v1.d");
    assert_false(rnp::path::exists(sec_path, true));
    assert_int_equal(0, RNP_MKDIR(sec_path.c_str(), S_IRWXU));
    assert_rnp_success(rnp_output_to_path(&output, sec_path.c_str()));
    assert_rnp_success(rnp_save_keys(ffi, "G10", output, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_true(rnp::path::exists(sec_path, true));
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;
    // start over (read from the saved locations)
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    // load pubring & secring
    assert_true(load_keys_kbx_g10(ffi, pub_path, sec_path));
    // check the counts
    count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(2, count);
    count = 0;
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(2, count);
    // cleanup
    rnp_ffi_destroy(ffi);

    // final cleanup
    clean_temp_dir(temp_dir);
    free(temp_dir);
}

TEST_F(rnp_tests, test_ffi_load_save_keys_to_utf8_path)
{
    const char kbx_pubring_utf8_filename[] = "pubring_\xC2\xA2.kbx";
    const char g10_secring_utf8_dirname[] = "private-keys-\xC2\xA2.d";
    const char utf8_filename[] = "bothring_\xC2\xA2.gpg";

    // setup FFI
    rnp_ffi_t ffi = NULL;
    test_ffi_init(&ffi);
    auto temp_dir = make_temp_dir();
    // save pubring && secring
    auto both_path = rnp::path::append(temp_dir, utf8_filename);
    assert_false(rnp::path::exists(both_path));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_path(&output, both_path.c_str()));
    assert_rnp_success(rnp_save_keys(
      ffi, "GPG", output, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_true(rnp::path::exists(both_path));
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;
    // start over (read from the saved locations)
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load both keyrings from the single file
    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_path(&input, both_path.c_str()));
    assert_non_null(input);
    assert_rnp_success(
      rnp_load_keys(ffi, "GPG", input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    rnp_input_destroy(input);
    input = NULL;
    // check the counts. We should get both secret and public keys, since public keys are
    // extracted from the secret ones.
    size_t count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(7, count);
    count = 0;
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(7, count);
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    // load pubring
    assert_true(load_keys_kbx_g10(
      ffi, "data/keyrings/3/pubring.kbx", "data/keyrings/3/private-keys-v1.d"));
    // save pubring
    auto pub_path = rnp::path::append(temp_dir, kbx_pubring_utf8_filename);
    assert_rnp_success(rnp_output_to_path(&output, pub_path.c_str()));
    assert_rnp_success(rnp_save_keys(ffi, "KBX", output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_true(rnp::path::exists(pub_path));
    // save secring
    auto sec_path = rnp::path::append(temp_dir, g10_secring_utf8_dirname);
    assert_false(rnp::path::exists(sec_path, true));
    assert_int_equal(0, RNP_MKDIR(sec_path.c_str(), S_IRWXU));
    assert_rnp_success(rnp_output_to_path(&output, sec_path.c_str()));
    assert_rnp_success(rnp_save_keys(ffi, "G10", output, RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_true(rnp::path::exists(sec_path, true));
    // cleanup
    rnp_ffi_destroy(ffi);
    ffi = NULL;
    // start over (read from the saved locations)
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    // load pubring & secring
    assert_true(load_keys_kbx_g10(ffi, pub_path, sec_path));
    // check the counts
    count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(2, count);
    count = 0;
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(2, count);
    // cleanup
    rnp_ffi_destroy(ffi);

    // final cleanup
    clean_temp_dir(temp_dir);
    free(temp_dir);
}

static size_t
get_longest_line_length(const std::string &str, const std::set<std::string> lines_to_skip)
{
    // eol could be \n or \r\n
    size_t index = 0;
    size_t max_len = 0;
    for (;;) {
        auto new_index = str.find('\n', index);
        if (new_index == std::string::npos) {
            break;
        }
        size_t line_length = new_index - index;
        if (str[new_index - 1] == '\r') {
            line_length--;
        }
        if (line_length > max_len &&
            lines_to_skip.find(str.substr(index, line_length)) == lines_to_skip.end()) {
            max_len = line_length;
        }
        index = new_index + 1;
    }
    return max_len;
}

TEST_F(rnp_tests, test_ffi_add_userid)
{
    rnp_ffi_t              ffi = NULL;
    char *                 results = NULL;
    size_t                 count = 0;
    rnp_uid_handle_t       uid;
    rnp_signature_handle_t sig;
    char *                 hash_alg_name = NULL;

    const char *new_userid = "my new userid <user@example.com>";
    const char *default_hash_userid = "default hash <user@example.com";
    const char *ripemd_hash_userid = "ripemd160 <user@example.com";

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, unused_getkeycb, NULL));

    // load our JSON
    auto json = file_to_str("data/test_ffi_json/generate-primary.json");

    // generate the keys
    assert_rnp_success(rnp_generate_key_json(ffi, json.c_str(), &results));
    assert_non_null(results);
    rnp_buffer_destroy(results);
    results = NULL;

    // check the key counts
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(1, count);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(1, count);

    rnp_key_handle_t key_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "test0", &key_handle));
    assert_non_null(key_handle);

    assert_rnp_success(rnp_key_get_uid_count(key_handle, &count));
    assert_int_equal(1, count);

    // protect+lock the key
    if (!sm2_enabled()) {
        assert_rnp_failure(rnp_key_protect(key_handle, "pass", "SM4", "CFB", "SM3", 999999));
        assert_rnp_success(
          rnp_key_protect(key_handle, "pass", "AES128", "CFB", "SHA256", 999999));
    } else {
        assert_rnp_success(rnp_key_protect(key_handle, "pass", "SM4", "CFB", "SM3", 999999));
    }
    assert_rnp_success(rnp_key_lock(key_handle));

    // add with NULL parameters
    assert_rnp_failure(rnp_key_add_uid(NULL, new_userid, NULL, 2147317200, 0x00, false));
    assert_rnp_failure(rnp_key_add_uid(key_handle, NULL, NULL, 2147317200, 0x00, false));

    // add the userid (no pass provider, should fail)
    assert_int_equal(
      RNP_ERROR_BAD_PASSWORD,
      rnp_key_add_uid(key_handle, new_userid, "SHA256", 2147317200, 0x00, false));

    // actually add the userid
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "pass"));
    // attempt to add empty uid
    assert_rnp_failure(rnp_key_add_uid(key_handle, "", NULL, 2147317200, 0, false));
    // add with default hash algorithm
    assert_rnp_success(
      rnp_key_add_uid(key_handle, default_hash_userid, NULL, 2147317200, 0, false));
    // check whether key was locked back
    bool locked = false;
    assert_rnp_success(rnp_key_is_locked(key_handle, &locked));
    assert_true(locked);
    // check if default hash was used
    assert_rnp_success(rnp_key_get_uid_handle_at(key_handle, 1, &uid));
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_success(rnp_signature_get_hash_alg(sig, &hash_alg_name));
    assert_int_equal(strcasecmp(hash_alg_name, DEFAULT_HASH_ALG), 0);
    rnp_buffer_destroy(hash_alg_name);
    hash_alg_name = NULL;
    assert_rnp_success(rnp_signature_handle_destroy(sig));
    assert_rnp_success(rnp_uid_handle_destroy(uid));

    assert_int_equal(
      RNP_SUCCESS, rnp_key_add_uid(key_handle, new_userid, "SHA256", 2147317200, 0x00, false));

    int uid_count_expected = 3;
    int res =
      rnp_key_add_uid(key_handle, ripemd_hash_userid, "RIPEMD160", 2147317200, 0, false);
    if (ripemd160_enabled()) {
        assert_rnp_success(res);
        uid_count_expected++;
    } else {
        assert_rnp_failure(res);
    }

    assert_rnp_success(rnp_key_get_uid_count(key_handle, &count));
    assert_int_equal(uid_count_expected, count);

    rnp_key_handle_t key_handle2 = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", new_userid, &key_handle2));
    assert_non_null(key_handle2);

    rnp_key_handle_destroy(key_handle);
    rnp_key_handle_destroy(key_handle2);
    rnp_ffi_destroy(ffi);
}

static void
test_ffi_init_sign_file_input(rnp_input_t *input, rnp_output_t *output)
{
    const char *plaintext = "this is some data that will be signed";

    // write out some data
    str_to_file("plaintext", plaintext);
    // create input+output
    assert_rnp_success(rnp_input_from_path(input, "plaintext"));
    assert_non_null(*input);
    assert_rnp_success(rnp_output_to_path(output, "signed"));
    assert_non_null(*output);
}

static void
test_ffi_init_sign_memory_input(rnp_input_t *input, rnp_output_t *output)
{
    const char *plaintext = "this is some data that will be signed";

    assert_rnp_success(
      rnp_input_from_memory(input, (uint8_t *) plaintext, strlen(plaintext), true));
    assert_non_null(*input);
    if (output) {
        assert_rnp_success(rnp_output_to_memory(output, 0));
        assert_non_null(*output);
    }
}

static void
test_ffi_init_verify_file_input(rnp_input_t *input, rnp_output_t *output)
{
    // create input+output
    assert_rnp_success(rnp_input_from_path(input, "signed"));
    assert_non_null(*input);
    assert_rnp_success(rnp_output_to_path(output, "recovered"));
    assert_non_null(*output);
}

static void
test_ffi_init_verify_detached_file_input(rnp_input_t *input, rnp_input_t *signature)
{
    assert_rnp_success(rnp_input_from_path(input, "plaintext"));
    assert_non_null(*input);
    assert_rnp_success(rnp_input_from_path(signature, "signed"));
    assert_non_null(*signature);
}

static void
test_ffi_init_verify_memory_input(rnp_input_t * input,
                                  rnp_output_t *output,
                                  uint8_t *     signed_buf,
                                  size_t        signed_len)
{
    // create input+output
    assert_rnp_success(rnp_input_from_memory(input, signed_buf, signed_len, false));
    assert_non_null(*input);
    assert_rnp_success(rnp_output_to_memory(output, 0));
    assert_non_null(*output);
}

static void
test_ffi_setup_signatures(rnp_ffi_t *ffi, rnp_op_sign_t *op)
{
    rnp_key_handle_t        key = NULL;
    rnp_op_sign_signature_t sig = NULL;
    // set signature times
    const uint32_t issued = 1516211899;   // Unix epoch, nowish
    const uint32_t expires = 1000000000;  // expires later
    const uint32_t issued2 = 1516211900;  // Unix epoch, nowish
    const uint32_t expires2 = 2000000000; // expires later

    assert_rnp_failure(rnp_op_sign_set_armor(NULL, true));
    assert_rnp_success(rnp_op_sign_set_armor(*op, true));
    assert_rnp_success(rnp_op_sign_set_hash(*op, "SHA256"));
    assert_rnp_failure(rnp_op_sign_set_creation_time(NULL, issued));
    assert_rnp_success(rnp_op_sign_set_creation_time(*op, issued));
    assert_rnp_failure(rnp_op_sign_set_expiration_time(NULL, expires));
    assert_rnp_success(rnp_op_sign_set_expiration_time(*op, expires));

    // set pass provider
    assert_rnp_success(
      rnp_ffi_set_pass_provider(*ffi, ffi_string_password_provider, (void *) "password"));

    // set first signature key
    assert_rnp_success(rnp_locate_key(*ffi, "userid", "key0-uid2", &key));
    assert_rnp_success(rnp_op_sign_add_signature(*op, key, NULL));
    assert_rnp_success(rnp_key_handle_destroy(key));
    key = NULL;
    // set second signature key
    assert_rnp_success(rnp_locate_key(*ffi, "userid", "key0-uid1", &key));
    assert_rnp_success(rnp_op_sign_add_signature(*op, key, &sig));
    assert_rnp_success(rnp_op_sign_signature_set_creation_time(sig, issued2));
    assert_rnp_success(rnp_op_sign_signature_set_expiration_time(sig, expires2));
    assert_rnp_success(rnp_op_sign_signature_set_hash(sig, "SHA512"));
    assert_rnp_success(rnp_key_handle_destroy(key));
}

static void
test_ffi_check_signatures(rnp_op_verify_t *verify)
{
    rnp_op_verify_signature_t sig;
    size_t                    sig_count;
    uint32_t                  sig_create;
    uint32_t                  sig_expires;
    char *                    hname = NULL;
    const uint32_t            issued = 1516211899;   // Unix epoch, nowish
    const uint32_t            expires = 1000000000;  // expires later
    const uint32_t            issued2 = 1516211900;  // Unix epoch, nowish
    const uint32_t            expires2 = 2000000000; // expires later

    assert_rnp_success(rnp_op_verify_get_signature_count(*verify, &sig_count));
    assert_int_equal(sig_count, 2);
    // first signature
    assert_rnp_success(rnp_op_verify_get_signature_at(*verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    assert_rnp_success(rnp_op_verify_signature_get_times(sig, &sig_create, &sig_expires));
    assert_int_equal(sig_create, issued);
    assert_int_equal(sig_expires, expires);
    assert_rnp_success(rnp_op_verify_signature_get_hash(sig, &hname));
    assert_string_equal(hname, "SHA256");
    rnp_buffer_destroy(hname);
    // second signature
    assert_rnp_success(rnp_op_verify_get_signature_at(*verify, 1, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    assert_rnp_success(rnp_op_verify_signature_get_times(sig, &sig_create, &sig_expires));
    assert_int_equal(sig_create, issued2);
    assert_int_equal(sig_expires, expires2);
    assert_rnp_success(rnp_op_verify_signature_get_hash(sig, &hname));
    assert_string_equal(hname, "SHA512");
    rnp_buffer_destroy(hname);
}

TEST_F(rnp_tests, test_ffi_signatures_memory)
{
    rnp_ffi_t       ffi = NULL;
    rnp_input_t     input = NULL;
    rnp_output_t    output = NULL;
    rnp_op_sign_t   op = NULL;
    rnp_op_verify_t verify;
    uint8_t *       signed_buf;
    size_t          signed_len;
    uint8_t *       verified_buf;
    size_t          verified_len;

    // init ffi
    test_ffi_init(&ffi);
    // init input
    test_ffi_init_sign_memory_input(&input, &output);
    // create signature operation
    assert_rnp_failure(rnp_op_sign_create(NULL, ffi, input, output));
    assert_rnp_failure(rnp_op_sign_create(&op, NULL, input, output));
    assert_rnp_failure(rnp_op_sign_create(&op, ffi, NULL, output));
    assert_rnp_failure(rnp_op_sign_create(&op, ffi, input, NULL));
    assert_rnp_success(rnp_op_sign_create(&op, ffi, input, output));
    // setup signature(s)
    test_ffi_setup_signatures(&ffi, &op);
    // execute the operation
    assert_rnp_failure(rnp_op_sign_execute(NULL));
    assert_rnp_success(rnp_op_sign_execute(op));
    // make sure the output file was created
    assert_rnp_failure(rnp_output_memory_get_buf(NULL, &signed_buf, &signed_len, true));
    assert_rnp_failure(rnp_output_memory_get_buf(output, NULL, &signed_len, true));
    assert_rnp_failure(rnp_output_memory_get_buf(output, &signed_buf, NULL, true));
    assert_rnp_success(rnp_output_memory_get_buf(output, &signed_buf, &signed_len, true));
    assert_non_null(signed_buf);
    assert_true(signed_len > 0);

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_op_sign_destroy(op));
    op = NULL;

    /* now verify */
    // make sure it is correctly armored
    assert_int_equal(memcmp(signed_buf, "-----BEGIN PGP MESSAGE-----", 27), 0);
    // create input and output
    test_ffi_init_verify_memory_input(&input, &output, signed_buf, signed_len);
    // call verify
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    // check signatures
    test_ffi_check_signatures(&verify);
    // get output
    assert_rnp_success(rnp_output_memory_get_buf(output, &verified_buf, &verified_len, true));
    assert_non_null(verified_buf);
    assert_true(verified_len > 0);
    // cleanup
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_ffi_destroy(ffi));
    rnp_buffer_destroy(signed_buf);
    rnp_buffer_destroy(verified_buf);
}

TEST_F(rnp_tests, test_ffi_signatures)
{
    rnp_ffi_t       ffi = NULL;
    rnp_input_t     input = NULL;
    rnp_output_t    output = NULL;
    rnp_op_sign_t   op = NULL;
    rnp_op_verify_t verify;

    // init ffi
    test_ffi_init(&ffi);
    // init file input
    test_ffi_init_sign_file_input(&input, &output);
    // create signature operation
    assert_rnp_success(rnp_op_sign_create(&op, ffi, input, output));
    // setup signature(s)
    test_ffi_setup_signatures(&ffi, &op);
    // execute the operation
    assert_rnp_success(rnp_op_sign_execute(op));
    // make sure the output file was created
    assert_true(rnp_file_exists("signed"));

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_op_sign_destroy(op));
    op = NULL;

    /* now verify */

    // create input and output
    test_ffi_init_verify_file_input(&input, &output);
    // call verify
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    // check signatures
    test_ffi_check_signatures(&verify);
    // cleanup
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_ffi_destroy(ffi));
    // check output
    assert_true(file_to_vec("recovered") == file_to_vec("plaintext"));
}

TEST_F(rnp_tests, test_ffi_signatures_detached_memory)
{
    rnp_ffi_t       ffi = NULL;
    rnp_input_t     input = NULL;
    rnp_input_t     signature = NULL;
    rnp_output_t    output = NULL;
    rnp_op_sign_t   op = NULL;
    rnp_op_verify_t verify;
    uint8_t *       signed_buf;
    size_t          signed_len;

    // init ffi
    test_ffi_init(&ffi);
    // init input
    test_ffi_init_sign_memory_input(&input, &output);
    // create signature operation
    assert_rnp_success(rnp_op_sign_detached_create(&op, ffi, input, output));
    // setup signature(s)
    test_ffi_setup_signatures(&ffi, &op);
    // execute the operation
    assert_rnp_success(rnp_op_sign_execute(op));
    assert_rnp_success(rnp_output_memory_get_buf(output, &signed_buf, &signed_len, true));
    assert_non_null(signed_buf);
    assert_true(signed_len > 0);

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_op_sign_destroy(op));
    op = NULL;

    /* now verify */
    // make sure it is correctly armored
    assert_int_equal(memcmp(signed_buf, "-----BEGIN PGP SIGNATURE-----", 29), 0);
    // create input and output
    test_ffi_init_sign_memory_input(&input, NULL);
    assert_rnp_success(rnp_input_from_memory(&signature, signed_buf, signed_len, true));
    assert_non_null(signature);
    // call verify
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, input, signature));
    assert_rnp_success(rnp_op_verify_execute(verify));
    // check signatures
    test_ffi_check_signatures(&verify);
    // cleanup
    rnp_buffer_destroy(signed_buf);
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_input_destroy(signature));
    signature = NULL;
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_signatures_detached)
{
    rnp_ffi_t       ffi = NULL;
    rnp_input_t     input = NULL;
    rnp_input_t     signature = NULL;
    rnp_output_t    output = NULL;
    rnp_op_sign_t   op = NULL;
    rnp_op_verify_t verify;

    // init ffi
    test_ffi_init(&ffi);
    // init file input
    test_ffi_init_sign_file_input(&input, &output);
    // create signature operation
    assert_rnp_success(rnp_op_sign_detached_create(&op, ffi, input, output));
    // setup signature(s)
    test_ffi_setup_signatures(&ffi, &op);
    // execute the operation
    assert_rnp_success(rnp_op_sign_execute(op));
    // make sure the output file was created
    assert_true(rnp_file_exists("signed"));

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_op_sign_destroy(op));
    op = NULL;

    /* now verify */

    // create input and output
    test_ffi_init_verify_detached_file_input(&input, &signature);
    // call verify
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, input, signature));
    assert_rnp_success(rnp_op_verify_execute(verify));
    // check signatures
    test_ffi_check_signatures(&verify);
    // cleanup
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_input_destroy(signature));
    signature = NULL;
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_signatures_dump)
{
    rnp_ffi_t       ffi = NULL;
    rnp_input_t     input = NULL;
    rnp_input_t     signature = NULL;
    rnp_op_verify_t verify;

    /* init ffi and inputs */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    load_keys_gpg(ffi, "data/test_stream_signatures/pub.asc");
    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_signatures/source.txt"));
    assert_rnp_success(
      rnp_input_from_path(&signature, "data/test_stream_signatures/source.txt.sig"));
    /* call verify detached to obtain signatures */
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, input, signature));
    assert_rnp_success(rnp_op_verify_execute(verify));
    /* get signature and check it */
    rnp_op_verify_signature_t sig;
    size_t                    sig_count;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sig_count));
    assert_int_equal(sig_count, 1);
    /* get signature handle  */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    rnp_signature_handle_t sighandle = NULL;
    assert_rnp_failure(rnp_op_verify_signature_get_handle(NULL, &sighandle));
    assert_rnp_failure(rnp_op_verify_signature_get_handle(sig, NULL));
    assert_rnp_success(rnp_op_verify_signature_get_handle(sig, &sighandle));
    assert_non_null(sighandle);
    /* check signature type */
    char *sigtype = NULL;
    assert_rnp_success(rnp_signature_get_type(sighandle, &sigtype));
    assert_string_equal(sigtype, "binary");
    rnp_buffer_destroy(sigtype);
    /* make sure it is valid */
    assert_rnp_success(rnp_signature_is_valid(sighandle, 0));
    /* cleanup, making sure that sighandle doesn't depend on verify */
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_input_destroy(signature));
    /* check whether getters work on sighandle: algorithm */
    char *alg = NULL;
    assert_rnp_success(rnp_signature_get_alg(sighandle, &alg));
    assert_non_null(alg);
    assert_string_equal(alg, "RSA");
    rnp_buffer_destroy(alg);
    /* keyid */
    char *keyid = NULL;
    assert_rnp_success(rnp_signature_get_keyid(sighandle, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "5873BD738E575398");
    rnp_buffer_destroy(keyid);
    /* creation time */
    uint32_t create = 0;
    assert_rnp_success(rnp_signature_get_creation(sighandle, &create));
    assert_int_equal(create, 1522241943);
    /* hash algorithm */
    assert_rnp_success(rnp_signature_get_hash_alg(sighandle, &alg));
    assert_non_null(alg);
    assert_string_equal(alg, "SHA256");
    rnp_buffer_destroy(alg);
    /* now dump signature packet to json */
    char *json = NULL;
    assert_rnp_success(rnp_signature_packet_to_json(sighandle, 0, &json));
    json_object *jso = json_tokener_parse(json);
    rnp_buffer_destroy(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_array));
    assert_int_equal(json_object_array_length(jso), 1);
    /* check the signature packet dump */
    json_object *pkt = json_object_array_get_idx(jso, 0);
    /* check helper functions */
    assert_false(check_json_field_int(pkt, "unknown", 4));
    assert_false(check_json_field_int(pkt, "version", 5));
    assert_true(check_json_field_int(pkt, "version", 4));
    assert_true(check_json_field_int(pkt, "type", 0));
    assert_true(check_json_field_str(pkt, "type.str", "Signature of a binary document"));
    assert_true(check_json_field_int(pkt, "algorithm", 1));
    assert_true(check_json_field_str(pkt, "algorithm.str", "RSA (Encrypt or Sign)"));
    assert_true(check_json_field_int(pkt, "hash algorithm", 8));
    assert_true(check_json_field_str(pkt, "hash algorithm.str", "SHA256"));
    assert_true(check_json_field_str(pkt, "lbits", "816e"));
    json_object *subpkts = NULL;
    assert_true(json_object_object_get_ex(pkt, "subpackets", &subpkts));
    assert_non_null(subpkts);
    assert_true(json_object_is_type(subpkts, json_type_array));
    assert_int_equal(json_object_array_length(subpkts), 3);
    /* subpacket 0 */
    json_object *subpkt = json_object_array_get_idx(subpkts, 0);
    assert_true(check_json_field_int(subpkt, "type", 33));
    assert_true(check_json_field_str(subpkt, "type.str", "issuer fingerprint"));
    assert_true(check_json_field_int(subpkt, "length", 21));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(
      check_json_field_str(subpkt, "fingerprint", "7a60e671179f9b920f6478a25873bd738e575398"));
    /* subpacket 1 */
    subpkt = json_object_array_get_idx(subpkts, 1);
    assert_true(check_json_field_int(subpkt, "type", 2));
    assert_true(check_json_field_str(subpkt, "type.str", "signature creation time"));
    assert_true(check_json_field_int(subpkt, "length", 4));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(check_json_field_int(subpkt, "creation time", 1522241943));
    /* subpacket 2 */
    subpkt = json_object_array_get_idx(subpkts, 2);
    assert_true(check_json_field_int(subpkt, "type", 16));
    assert_true(check_json_field_str(subpkt, "type.str", "issuer key ID"));
    assert_true(check_json_field_int(subpkt, "length", 8));
    assert_true(check_json_field_bool(subpkt, "hashed", false));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(check_json_field_str(subpkt, "issuer keyid", "5873bd738e575398"));
    json_object_put(jso);
    rnp_signature_handle_destroy(sighandle);
    /* check text-mode detached signature */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_signatures/source.txt"));
    assert_rnp_success(
      rnp_input_from_path(&signature, "data/test_stream_signatures/source.txt.text.sig"));
    /* call verify detached to obtain signatures */
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, input, signature));
    assert_rnp_success(rnp_op_verify_execute(verify));
    /* get signature and check it */
    sig_count = 0;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sig_count));
    assert_int_equal(sig_count, 1);
    /* get signature handle  */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    assert_rnp_success(rnp_op_verify_signature_get_handle(sig, &sighandle));
    assert_non_null(sighandle);
    /* check signature type */
    assert_rnp_success(rnp_signature_get_type(sighandle, &sigtype));
    assert_string_equal(sigtype, "text");
    rnp_buffer_destroy(sigtype);
    /* make sure it is valid */
    assert_rnp_success(rnp_signature_is_valid(sighandle, 0));
    /* cleanup, making sure that sighandle doesn't depend on verify */
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_input_destroy(signature));
    /* check whether getters work on sighandle: algorithm */
    assert_rnp_success(rnp_signature_get_alg(sighandle, &alg));
    assert_non_null(alg);
    assert_string_equal(alg, "RSA");
    rnp_buffer_destroy(alg);
    /* keyid */
    assert_rnp_success(rnp_signature_get_keyid(sighandle, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "5873BD738E575398");
    rnp_buffer_destroy(keyid);
    /* creation time */
    assert_rnp_success(rnp_signature_get_creation(sighandle, &create));
    assert_int_equal(create, 1608118321);
    /* hash algorithm */
    assert_rnp_success(rnp_signature_get_hash_alg(sighandle, &alg));
    assert_non_null(alg);
    assert_string_equal(alg, "SHA256");
    rnp_buffer_destroy(alg);
    /* now dump signature packet to json */
    assert_rnp_success(rnp_signature_packet_to_json(sighandle, 0, &json));
    jso = json_tokener_parse(json);
    rnp_buffer_destroy(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_array));
    assert_int_equal(json_object_array_length(jso), 1);
    /* check the signature packet dump */
    pkt = json_object_array_get_idx(jso, 0);
    /* check helper functions */
    assert_false(check_json_field_int(pkt, "unknown", 4));
    assert_false(check_json_field_int(pkt, "version", 5));
    assert_true(check_json_field_int(pkt, "version", 4));
    assert_true(check_json_field_int(pkt, "type", 1));
    assert_true(
      check_json_field_str(pkt, "type.str", "Signature of a canonical text document"));
    assert_true(check_json_field_int(pkt, "algorithm", 1));
    assert_true(check_json_field_str(pkt, "algorithm.str", "RSA (Encrypt or Sign)"));
    assert_true(check_json_field_int(pkt, "hash algorithm", 8));
    assert_true(check_json_field_str(pkt, "hash algorithm.str", "SHA256"));
    assert_true(check_json_field_str(pkt, "lbits", "1037"));
    subpkts = NULL;
    assert_true(json_object_object_get_ex(pkt, "subpackets", &subpkts));
    assert_non_null(subpkts);
    assert_true(json_object_is_type(subpkts, json_type_array));
    assert_int_equal(json_object_array_length(subpkts), 3);
    /* subpacket 0 */
    subpkt = json_object_array_get_idx(subpkts, 0);
    assert_true(check_json_field_int(subpkt, "type", 33));
    assert_true(check_json_field_str(subpkt, "type.str", "issuer fingerprint"));
    assert_true(check_json_field_int(subpkt, "length", 21));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(
      check_json_field_str(subpkt, "fingerprint", "7a60e671179f9b920f6478a25873bd738e575398"));
    /* subpacket 1 */
    subpkt = json_object_array_get_idx(subpkts, 1);
    assert_true(check_json_field_int(subpkt, "type", 2));
    assert_true(check_json_field_str(subpkt, "type.str", "signature creation time"));
    assert_true(check_json_field_int(subpkt, "length", 4));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(check_json_field_int(subpkt, "creation time", 1608118321));
    /* subpacket 2 */
    subpkt = json_object_array_get_idx(subpkts, 2);
    assert_true(check_json_field_int(subpkt, "type", 16));
    assert_true(check_json_field_str(subpkt, "type.str", "issuer key ID"));
    assert_true(check_json_field_int(subpkt, "length", 8));
    assert_true(check_json_field_bool(subpkt, "hashed", false));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(check_json_field_str(subpkt, "issuer keyid", "5873bd738e575398"));
    json_object_put(jso);
    rnp_signature_handle_destroy(sighandle);

    /* attempt to validate a timestamp signature instead of detached */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_signatures/source.txt"));
    assert_rnp_success(
      rnp_input_from_path(&signature, "data/test_stream_signatures/signature-timestamp.asc"));
    /* call verify detached to obtain signatures */
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, input, signature));
    assert_int_equal(rnp_op_verify_execute(verify), RNP_ERROR_SIGNATURE_INVALID);
    /* get signature and check it */
    sig_count = 0;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sig_count));
    assert_int_equal(sig_count, 1);
    /* get signature handle  */
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_int_equal(rnp_op_verify_signature_get_status(sig), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_op_verify_signature_get_handle(sig, &sighandle));
    assert_non_null(sighandle);
    /* check signature type */
    assert_rnp_success(rnp_signature_get_type(sighandle, &sigtype));
    assert_string_equal(sigtype, "timestamp");
    rnp_buffer_destroy(sigtype);
    /* make sure validity status could be checked */
    assert_int_equal(rnp_signature_is_valid(sighandle, 0), RNP_ERROR_SIGNATURE_INVALID);
    /* cleanup, making sure that sighandle doesn't depend on verify */
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_input_destroy(signature));
    /* check whether getters work on sighandle: algorithm */
    assert_rnp_success(rnp_signature_get_alg(sighandle, &alg));
    assert_non_null(alg);
    assert_string_equal(alg, "DSA");
    rnp_buffer_destroy(alg);
    /* keyid */
    assert_rnp_success(rnp_signature_get_keyid(sighandle, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "2D727CC768697734");
    rnp_buffer_destroy(keyid);
    /* creation time */
    assert_rnp_success(rnp_signature_get_creation(sighandle, &create));
    assert_int_equal(create, 1535389094);
    /* hash algorithm */
    assert_rnp_success(rnp_signature_get_hash_alg(sighandle, &alg));
    assert_non_null(alg);
    assert_string_equal(alg, "SHA512");
    rnp_buffer_destroy(alg);
    /* now dump signature packet to json */
    assert_rnp_success(rnp_signature_packet_to_json(sighandle, 0, &json));
    jso = json_tokener_parse(json);
    rnp_buffer_destroy(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_array));
    assert_int_equal(json_object_array_length(jso), 1);
    /* check the signature packet dump */
    pkt = json_object_array_get_idx(jso, 0);
    /* check helper functions */
    assert_false(check_json_field_int(pkt, "unknown", 4));
    assert_false(check_json_field_int(pkt, "version", 5));
    assert_true(check_json_field_int(pkt, "version", 4));
    assert_true(check_json_field_int(pkt, "type", 0x40));
    assert_true(check_json_field_str(pkt, "type.str", "Timestamp signature"));
    assert_true(check_json_field_int(pkt, "algorithm", 17));
    assert_true(check_json_field_str(pkt, "algorithm.str", "DSA"));
    assert_true(check_json_field_int(pkt, "hash algorithm", 10));
    assert_true(check_json_field_str(pkt, "hash algorithm.str", "SHA512"));
    assert_true(check_json_field_str(pkt, "lbits", "2727"));
    subpkts = NULL;
    assert_true(json_object_object_get_ex(pkt, "subpackets", &subpkts));
    assert_non_null(subpkts);
    assert_true(json_object_is_type(subpkts, json_type_array));
    assert_int_equal(json_object_array_length(subpkts), 7);
    /* subpacket 0 */
    subpkt = json_object_array_get_idx(subpkts, 0);
    assert_true(check_json_field_int(subpkt, "type", 2));
    assert_true(check_json_field_str(subpkt, "type.str", "signature creation time"));
    assert_true(check_json_field_int(subpkt, "length", 4));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", true));
    assert_true(check_json_field_int(subpkt, "creation time", 1535389094));
    /* subpacket 1 */
    subpkt = json_object_array_get_idx(subpkts, 1);
    assert_true(check_json_field_int(subpkt, "type", 7));
    assert_true(check_json_field_str(subpkt, "type.str", "revocable"));
    assert_true(check_json_field_int(subpkt, "length", 1));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", true));
    assert_true(check_json_field_bool(subpkt, "revocable", false));
    /* subpacket 2 */
    subpkt = json_object_array_get_idx(subpkts, 2);
    assert_true(check_json_field_int(subpkt, "type", 16));
    assert_true(check_json_field_str(subpkt, "type.str", "issuer key ID"));
    assert_true(check_json_field_int(subpkt, "length", 8));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", true));
    assert_true(check_json_field_str(subpkt, "issuer keyid", "2d727cc768697734"));
    /* subpacket 3 */
    subpkt = json_object_array_get_idx(subpkts, 3);
    assert_true(check_json_field_int(subpkt, "type", 20));
    assert_true(check_json_field_str(subpkt, "type.str", "notation data"));
    assert_true(check_json_field_int(subpkt, "length", 51));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(check_json_field_bool(subpkt, "human", true));
    assert_true(check_json_field_str(subpkt, "name", "serialnumber@dots.testdomain.test"));
    assert_true(check_json_field_str(subpkt, "value", "TEST000001"));
    /* subpacket 4 */
    subpkt = json_object_array_get_idx(subpkts, 4);
    assert_true(check_json_field_int(subpkt, "type", 26));
    assert_true(check_json_field_str(subpkt, "type.str", "policy URI"));
    assert_true(check_json_field_int(subpkt, "length", 44));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(
      check_json_field_str(subpkt, "uri", "https://policy.testdomain.test/timestamping/"));
    /* subpacket 5 */
    subpkt = json_object_array_get_idx(subpkts, 5);
    assert_true(check_json_field_int(subpkt, "type", 32));
    assert_true(check_json_field_str(subpkt, "type.str", "embedded signature"));
    assert_true(check_json_field_int(subpkt, "length", 105));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", true));
    json_object *embsig = NULL;
    assert_true(json_object_object_get_ex(subpkt, "signature", &embsig));
    assert_true(check_json_field_int(embsig, "version", 4));
    assert_true(check_json_field_int(embsig, "type", 0));
    assert_true(check_json_field_str(embsig, "type.str", "Signature of a binary document"));
    assert_true(check_json_field_int(embsig, "algorithm", 17));
    assert_true(check_json_field_str(embsig, "algorithm.str", "DSA"));
    assert_true(check_json_field_int(embsig, "hash algorithm", 10));
    assert_true(check_json_field_str(embsig, "hash algorithm.str", "SHA512"));
    assert_true(check_json_field_str(embsig, "lbits", "a386"));
    /* subpacket 6 */
    subpkt = json_object_array_get_idx(subpkts, 6);
    assert_true(check_json_field_int(subpkt, "type", 33));
    assert_true(check_json_field_str(subpkt, "type.str", "issuer fingerprint"));
    assert_true(check_json_field_int(subpkt, "length", 21));
    assert_true(check_json_field_bool(subpkt, "hashed", true));
    assert_true(check_json_field_bool(subpkt, "critical", false));
    assert_true(
      check_json_field_str(subpkt, "fingerprint", "a0ff4590bb6122edef6e3c542d727cc768697734"));
    json_object_put(jso);
    rnp_signature_handle_destroy(sighandle);

    /* cleanup ffi */
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_locate_key)
{
    rnp_ffi_t ffi = NULL;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load our keyrings
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));

    // edge cases
    {
        rnp_key_handle_t key = NULL;
        assert_rnp_failure(rnp_locate_key(NULL, "keyid", "7BC6709B15C23A4A", &key));
        assert_rnp_failure(rnp_locate_key(ffi, NULL, "7BC6709B15C23A4A", &key));
        assert_rnp_failure(rnp_locate_key(ffi, "keyid", NULL, &key));
        assert_rnp_failure(rnp_locate_key(ffi, "keyid", "7BC6709B15C23A4A", NULL));
        assert_rnp_failure(rnp_locate_key(ffi, "wrong", "7BC6709B15C23A4A", &key));
        assert_rnp_failure(rnp_locate_key(ffi, "keyid", "C6709B15C23A4A", &key));
        assert_rnp_failure(
          rnp_locate_key(ffi, "fingerprint", "5A3CBF583AA80A2CCC53AA7BC6709B15C23A4A", &key));
        assert_rnp_failure(
          rnp_locate_key(ffi, "grip", "D6A0800A3FACDE0C0EB60B16B3669ED380FDFA", &key));
        assert_rnp_failure(rnp_locate_key(ffi, "keyid", "0x7BC6 709B\r15C2 3A4A\n", &key));
        assert_rnp_success(rnp_locate_key(ffi, "keyid", "0x7BC6 709B\t15C2 3A4A\t", &key));
        assert_non_null(key);
        rnp_key_handle_destroy(key);
    }
    // keyid
    {
        static const char *ids[] = {"7BC6709B15C23A4A",
                                    "1ED63EE56FADC34D",
                                    "1D7E8A5393C997A8",
                                    "8A05B89FAD5ADED1",
                                    "2FCADF05FFA501BB",
                                    "54505A936A4A970E",
                                    "326EF111425D14A5"};
        for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
            const char *     id = ids[i];
            rnp_key_handle_t key = NULL;
            assert_rnp_success(rnp_locate_key(ffi, "keyid", id, &key));
            assert_non_null(key);
            rnp_key_handle_destroy(key);
        }
        // invalid - value did not change
        {
            rnp_key_handle_t key = (rnp_key_handle_t) 0x111;
            assert_rnp_failure(rnp_locate_key(ffi, "keyid", "invalid-keyid", &key));
            assert_true(key == (rnp_key_handle_t) 0x111);
        }
        // valid but non-existent - null returned
        {
            rnp_key_handle_t key = (rnp_key_handle_t) 0x111;
            assert_rnp_success(rnp_locate_key(ffi, "keyid", "AAAAAAAAAAAAAAAA", &key));
            assert_null(key);
        }
    }

    // userid
    {
        static const char *ids[] = {
          "key0-uid0", "key0-uid1", "key0-uid2", "key1-uid0", "key1-uid2", "key1-uid1"};
        for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
            const char *     id = ids[i];
            rnp_key_handle_t key = NULL;
            assert_rnp_success(rnp_locate_key(ffi, "userid", id, &key));
            assert_non_null(key);
            rnp_key_handle_destroy(key);
        }
        // valid but non-existent
        {
            rnp_key_handle_t key = (rnp_key_handle_t) 0x111;
            assert_rnp_success(rnp_locate_key(ffi, "userid", "bad-userid", &key));
            assert_null(key);
        }
    }

    // fingerprint
    {
        static const char *ids[] = {"E95A3CBF583AA80A2CCC53AA7BC6709B15C23A4A",
                                    "E332B27CAF4742A11BAA677F1ED63EE56FADC34D",
                                    "C5B15209940A7816A7AF3FB51D7E8A5393C997A8",
                                    "5CD46D2A0BD0B8CFE0B130AE8A05B89FAD5ADED1",
                                    "BE1C4AB951F4C2F6B604C7F82FCADF05FFA501BB",
                                    "A3E94DE61A8CB229413D348E54505A936A4A970E",
                                    "57F8ED6E5C197DB63C60FFAF326EF111425D14A5"};
        for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
            const char *     id = ids[i];
            rnp_key_handle_t key = NULL;
            assert_rnp_success(rnp_locate_key(ffi, "fingerprint", id, &key));
            assert_non_null(key);
            rnp_key_handle_destroy(key);
        }
        // invalid
        {
            rnp_key_handle_t key = (rnp_key_handle_t) 0x111;
            assert_rnp_failure(rnp_locate_key(ffi, "fingerprint", "invalid-fpr", &key));
            assert_true(key == (rnp_key_handle_t) 0x111);
        }
        // valid but non-existent
        {
            rnp_key_handle_t key = (rnp_key_handle_t) 0x111;
            assert_rnp_success(rnp_locate_key(
              ffi, "fingerprint", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", &key));
            assert_null(key);
        }
    }

    // grip
    {
        static const char *ids[] = {"66D6A0800A3FACDE0C0EB60B16B3669ED380FDFA",
                                    "D9839D61EDAF0B3974E0A4A341D6E95F3479B9B7",
                                    "B1CC352FEF9A6BD4E885B5351840EF9306D635F0",
                                    "E7C8860B70DC727BED6DB64C633683B41221BB40",
                                    "B2A7F6C34AA2C15484783E9380671869A977A187",
                                    "43C01D6D96BE98C3C87FE0F175870ED92DE7BE45",
                                    "8082FE753013923972632550838A5F13D81F43B9"};
        for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
            const char *     id = ids[i];
            rnp_key_handle_t key = NULL;
            assert_rnp_success(rnp_locate_key(ffi, "grip", id, &key));
            assert_non_null(key);
            rnp_key_handle_destroy(key);
        }
        // invalid
        {
            rnp_key_handle_t key = (rnp_key_handle_t) 0x111;
            assert_rnp_failure(rnp_locate_key(ffi, "grip", "invalid-fpr", &key));
            assert_true(key == (rnp_key_handle_t) 0x111);
        }
        // valid but non-existent
        {
            rnp_key_handle_t key = (rnp_key_handle_t) 0x111;
            assert_rnp_success(
              rnp_locate_key(ffi, "grip", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", &key));
            assert_null(key);
        }
    }

    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_signatures_detached_memory_g10)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_input_t      input_sig = NULL;
    rnp_output_t     output = NULL;
    rnp_key_handle_t key = NULL;
    rnp_op_sign_t    opsign = NULL;
    rnp_op_verify_t  opverify = NULL;
    const char *     data = "my data";
    uint8_t *        sig = NULL;
    size_t           sig_len = 0;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "G10"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    // load our keyrings
    assert_true(load_keys_kbx_g10(
      ffi, "data/keyrings/3/pubring.kbx", "data/keyrings/3/private-keys-v1.d"));

    // find our signing key
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "4BE147BB22DF1E60", &key));
    assert_non_null(key);

    // create our input
    assert_rnp_success(rnp_input_from_memory(&input, (uint8_t *) data, strlen(data), false));
    assert_non_null(input);
    // create our output
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_non_null(output);
    // create the signing operation
    assert_rnp_success(rnp_op_sign_detached_create(&opsign, ffi, input, output));
    assert_non_null(opsign);

    // add the signer
    assert_rnp_failure(rnp_op_sign_add_signature(NULL, key, NULL));
    assert_rnp_success(rnp_op_sign_add_signature(opsign, key, NULL));
    // execute the signing operation
    assert_rnp_success(rnp_op_sign_execute(opsign));
    // get the resulting signature
    assert_rnp_success(rnp_output_memory_get_buf(output, &sig, &sig_len, true));
    assert_non_null(sig);
    assert_int_not_equal(0, sig_len);
    // cleanup
    rnp_op_sign_destroy(opsign);
    opsign = NULL;
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;

    // verify
    // create our data input
    assert_rnp_success(rnp_input_from_memory(&input, (uint8_t *) data, strlen(data), false));
    assert_non_null(input);
    // create our signature input
    assert_rnp_success(rnp_input_from_memory(&input_sig, sig, sig_len, true));
    assert_non_null(input_sig);
    // create our operation
    assert_rnp_success(rnp_op_verify_detached_create(&opverify, ffi, input, input_sig));
    assert_non_null(opverify);
    // execute the verification
    assert_rnp_success(rnp_op_verify_execute(opverify));
    // cleanup
    rnp_op_verify_destroy(opverify);
    opverify = NULL;
    rnp_input_destroy(input);
    input = NULL;
    rnp_input_destroy(input_sig);
    input_sig = NULL;

    // verify (tamper with signature)
    // create our data input
    assert_rnp_success(rnp_input_from_memory(&input, (uint8_t *) data, strlen(data), false));
    assert_non_null(input);
    // create our signature input
    sig[sig_len - 5] ^= 0xff;
    assert_rnp_success(rnp_input_from_memory(&input_sig, sig, sig_len, true));
    assert_non_null(input_sig);
    // create our operation
    assert_rnp_success(rnp_op_verify_detached_create(&opverify, ffi, input, input_sig));
    assert_non_null(opverify);
    // execute the verification
    assert_rnp_failure(rnp_op_verify_execute(opverify));
    // cleanup
    rnp_op_verify_destroy(opverify);
    opverify = NULL;
    rnp_input_destroy(input);
    input = NULL;
    rnp_input_destroy(input_sig);
    input_sig = NULL;

    // cleanup
    rnp_buffer_destroy(sig);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_enarmor_dearmor)
{
    std::string data;

    // enarmor plain message
    const std::string msg("this is a test");
    data.clear();
    {
        uint8_t *    buf = NULL;
        size_t       buf_size = 0;
        rnp_input_t  input = NULL;
        rnp_output_t output = NULL;

        assert_rnp_success(
          rnp_input_from_memory(&input, (const uint8_t *) msg.data(), msg.size(), true));
        assert_rnp_success(rnp_output_to_memory(&output, 0));

        assert_rnp_success(rnp_enarmor(input, output, "message"));

        rnp_output_memory_get_buf(output, &buf, &buf_size, false);
        data = std::string(buf, buf + buf_size);
        assert_true(starts_with(data, "-----BEGIN PGP MESSAGE-----\r\n"));
        assert_true(ends_with(data, "-----END PGP MESSAGE-----\r\n"));

        rnp_input_destroy(input);
        rnp_output_destroy(output);
    }
    {
        uint8_t *    buf = NULL;
        size_t       buf_size = 0;
        rnp_input_t  input = NULL;
        rnp_output_t output = NULL;

        assert_rnp_success(
          rnp_input_from_memory(&input, (const uint8_t *) data.data(), data.size(), true));
        assert_rnp_success(rnp_output_to_memory(&output, 0));

        assert_rnp_success(rnp_dearmor(input, output));

        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_size, false));
        std::string dearmored(buf, buf + buf_size);
        assert_true(msg == dearmored);

        rnp_input_destroy(input);
        rnp_output_destroy(output);
    }

    // enarmor public key
    data.clear();
    {
        uint8_t *    buf = NULL;
        size_t       buf_size = 0;
        rnp_input_t  input = NULL;
        rnp_output_t output = NULL;

        // enarmor
        assert_rnp_success(rnp_input_from_path(&input, "data/keyrings/1/pubring.gpg"));
        assert_rnp_success(rnp_output_to_memory(&output, 0));

        assert_rnp_success(rnp_enarmor(input, output, NULL));

        rnp_output_memory_get_buf(output, &buf, &buf_size, false);
        data = std::string(buf, buf + buf_size);
        assert_true(starts_with(data, "-----BEGIN PGP PUBLIC KEY BLOCK-----\r\n"));
        assert_true(ends_with(data, "-----END PGP PUBLIC KEY BLOCK-----\r\n"));

        rnp_input_destroy(input);
        rnp_output_destroy(output);
    }
    // dearmor public key
    {
        uint8_t *    buf = NULL;
        size_t       buf_size = 0;
        rnp_input_t  input = NULL;
        rnp_output_t output = NULL;

        assert_rnp_success(
          rnp_input_from_memory(&input, (const uint8_t *) data.data(), data.size(), true));
        assert_rnp_success(rnp_output_to_memory(&output, 0));

        assert_rnp_success(rnp_dearmor(input, output));

        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_size, false));
        std::string   dearmored(buf, buf + buf_size);
        std::ifstream inf("data/keyrings/1/pubring.gpg", std::ios::binary | std::ios::ate);
        std::string   from_disk(inf.tellg(), ' ');
        inf.seekg(0);
        inf.read(&from_disk[0], from_disk.size());
        inf.close();
        assert_true(dearmored == from_disk);

        rnp_input_destroy(input);
        rnp_output_destroy(output);
    }
    // test truncated armored data
    {
        std::ifstream keyf("data/test_stream_key_load/rsa-rsa-pub.asc",
                           std::ios::binary | std::ios::ate);
        std::string   keystr(keyf.tellg(), ' ');
        keyf.seekg(0);
        keyf.read(&keystr[0], keystr.size());
        keyf.close();
        for (size_t sz = keystr.size() - 2; sz > 0; sz--) {
            rnp_input_t  input = NULL;
            rnp_output_t output = NULL;

            assert_rnp_success(
              rnp_input_from_memory(&input, (const uint8_t *) keystr.data(), sz, true));
            assert_rnp_success(rnp_output_to_memory(&output, 0));
            assert_rnp_failure(rnp_dearmor(input, output));

            rnp_input_destroy(input);
            rnp_output_destroy(output);
        }
    }
}

TEST_F(rnp_tests, test_ffi_dearmor_edge_cases)
{
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/long_header_line.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/empty_header_line.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(rnp_input_from_path(
      &input, "data/test_stream_armor/64k_whitespace_before_armored_message.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_dearmor(input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* Armor header starts and fits in the first 1024 bytes of the input. Prepended by
     * whitespaces. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/1024_peek_buf.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/blank_line_with_whitespace.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/duplicate_header_line.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/long_header_line_1024.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/long_header_line_64k.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/long_header_nameline_64k.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* Armored message encoded in a single >64k text line */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/message_64k_oneline.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 68647);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/wrong_header_line.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* invalid, > 127 (negative char), preceding the armor header - just warning */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/wrong_chars_header.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dearmor(input, output));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 2226);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* invalid, > 127, base64 chars at positions 1..4 */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/wrong_chars_base64_1.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_dearmor(input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/wrong_chars_base64_2.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_dearmor(input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/wrong_chars_base64_3.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_dearmor(input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/wrong_chars_base64_4.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_dearmor(input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* invalid, > 127 base64 char in the crc */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/wrong_chars_crc.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_dearmor(input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* too short armor header */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/too_short_header.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_dearmor(input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* wrong base64 padding */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_armor/wrong_b64_trailer.asc"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_dearmor(input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
}

TEST_F(rnp_tests, test_ffi_customized_enarmor)
{
    rnp_input_t           input = NULL;
    rnp_output_t          output = NULL;
    rnp_output_t          armor_layer = NULL;
    const std::string     msg("this is a test long enough to have more than 76 characters in "
                          "enarmored representation");
    std::set<std::string> lines_to_skip{"-----BEGIN PGP MESSAGE-----",
                                        "-----END PGP MESSAGE-----"};

    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_output_to_armor(output, &armor_layer, "message"));
    // should fail when trying to set line length on non-armor output
    assert_rnp_failure(rnp_output_armor_set_line_length(output, 64));
    // should fail when trying to set zero line length
    assert_rnp_failure(rnp_output_armor_set_line_length(armor_layer, 0));
    // should fail when trying to set line length less than the minimum allowed 16
    assert_rnp_failure(rnp_output_armor_set_line_length(armor_layer, 15));
    assert_rnp_success(rnp_output_armor_set_line_length(armor_layer, 16));
    assert_rnp_success(rnp_output_armor_set_line_length(armor_layer, 76));
    // should fail when trying to set line length greater than the maximum allowed 76
    assert_rnp_failure(rnp_output_armor_set_line_length(armor_layer, 77));
    assert_rnp_success(rnp_output_destroy(armor_layer));
    assert_rnp_success(rnp_output_destroy(output));

    for (size_t llen = 16; llen <= 76; llen++) {
        std::string data;
        uint8_t *   buf = NULL;
        size_t      buf_size = 0;

        input = NULL;
        output = NULL;
        armor_layer = NULL;
        assert_rnp_success(
          rnp_input_from_memory(&input, (const uint8_t *) msg.data(), msg.size(), true));
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_rnp_success(rnp_output_to_armor(output, &armor_layer, "message"));
        assert_rnp_success(rnp_output_armor_set_line_length(armor_layer, llen));
        assert_rnp_success(rnp_output_pipe(input, armor_layer));
        assert_rnp_success(rnp_output_finish(armor_layer));
        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_size, false));
        data = std::string(buf, buf + buf_size);
        auto effective_llen = get_longest_line_length(data, lines_to_skip);
        assert_int_equal(llen / 4, effective_llen / 4);
        assert_true(llen >= effective_llen);
        assert_rnp_success(rnp_input_destroy(input));
        assert_rnp_success(rnp_output_destroy(armor_layer));
        assert_rnp_success(rnp_output_destroy(output));

        // test that the dearmored message is correct
        assert_rnp_success(
          rnp_input_from_memory(&input, (const uint8_t *) data.data(), data.size(), true));
        assert_rnp_success(rnp_output_to_memory(&output, 0));

        assert_rnp_success(rnp_dearmor(input, output));

        assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_size, false));
        std::string dearmored(buf, buf + buf_size);
        assert_true(msg == dearmored);

        assert_rnp_success(rnp_input_destroy(input));
        assert_rnp_success(rnp_output_destroy(output));
    }
}

TEST_F(rnp_tests, test_ffi_version)
{
    const uint32_t version = rnp_version();
    const uint32_t major = rnp_version_major(version);
    const uint32_t minor = rnp_version_minor(version);
    const uint32_t patch = rnp_version_patch(version);

    // reconstruct the version string
    assert_string_equal(fmt("%d.%d.%d", major, minor, patch).c_str(), rnp_version_string());

    // full version string should probably be at least as long as regular version string
    assert_true(strlen(rnp_version_string_full()) >= strlen(rnp_version_string()));

    // reconstruct the version value
    assert_int_equal(version, rnp_version_for(major, minor, patch));

    // check out-of-range handling
    assert_int_equal(0, rnp_version_for(1024, 0, 0));
    assert_int_equal(0, rnp_version_for(0, 1024, 0));
    assert_int_equal(0, rnp_version_for(0, 0, 1024));

    // check component extraction again
    assert_int_equal(rnp_version_major(rnp_version_for(5, 4, 3)), 5);
    assert_int_equal(rnp_version_minor(rnp_version_for(5, 4, 3)), 4);
    assert_int_equal(rnp_version_patch(rnp_version_for(5, 4, 3)), 3);

    // simple comparisons
    assert_true(rnp_version_for(1, 0, 1) > rnp_version_for(1, 0, 0));
    assert_true(rnp_version_for(1, 1, 0) > rnp_version_for(1, 0, 1023));
    assert_true(rnp_version_for(2, 0, 0) > rnp_version_for(1, 1023, 1023));

    // commit timestamp
    const uint64_t timestamp = rnp_version_commit_timestamp();
    assert_true(!timestamp || (timestamp >= 1639439116));
}

TEST_F(rnp_tests, test_ffi_backend_version)
{
    assert_non_null(rnp_backend_string());
    assert_non_null(rnp_backend_version());

    assert_true(strlen(rnp_backend_string()) > 0 && strlen(rnp_backend_string()) < 255);
    assert_true(strlen(rnp_backend_version()) > 0 && strlen(rnp_backend_version()) < 255);
}

void check_loaded_keys(const char *                    format,
                       bool                            armored,
                       uint8_t *                       buf,
                       size_t                          buf_len,
                       const char *                    id_type,
                       const std::vector<std::string> &expected_ids,
                       bool                            secret);

TEST_F(rnp_tests, test_ffi_key_export_customized_enarmor)
{
    rnp_ffi_t             ffi = NULL;
    rnp_output_t          output = NULL;
    rnp_output_t          armor_layer = NULL;
    rnp_key_handle_t      key = NULL;
    uint8_t *             buf = NULL;
    size_t                buf_len = 0;
    std::set<std::string> lines_to_skip{"-----BEGIN PGP PUBLIC KEY BLOCK-----",
                                        "-----END PGP PUBLIC KEY BLOCK-----",
                                        "-----BEGIN PGP PRIVATE KEY BLOCK-----",
                                        "-----END PGP PRIVATE KEY BLOCK-----"};
    // setup FFI
    test_ffi_init(&ffi);

    for (size_t llen = 16; llen <= 76; llen++) {
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
            assert_rnp_success(rnp_output_to_armor(output, &armor_layer, "public key"));
            assert_non_null(armor_layer);
            assert_rnp_success(rnp_output_armor_set_line_length(armor_layer, llen));

            // export
            assert_rnp_success(rnp_key_export(key, armor_layer, RNP_KEY_EXPORT_PUBLIC));
            assert_rnp_success(rnp_output_finish(armor_layer));
            // get output
            buf = NULL;
            assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
            assert_non_null(buf);
            std::string data = std::string(buf, buf + buf_len);
            auto        effective_llen = get_longest_line_length(data, lines_to_skip);
            assert_int_equal(llen / 4, effective_llen / 4);
            assert_true(llen >= effective_llen);

            // check results
            check_loaded_keys("GPG", true, buf, buf_len, "keyid", {"2FCADF05FFA501BB"}, false);

            // cleanup
            rnp_output_destroy(armor_layer);
            rnp_output_destroy(output);
            rnp_key_handle_destroy(key);
        }

        // primary sec only
        {
            // locate key
            key = NULL;
            assert_rnp_success(rnp_locate_key(ffi, "keyid", "2FCADF05FFA501BB", &key));
            assert_non_null(key);

            // create output
            output = NULL;
            assert_rnp_success(rnp_output_to_memory(&output, 0));
            assert_non_null(output);
            assert_rnp_success(rnp_output_to_armor(output, &armor_layer, "secret key"));
            assert_non_null(armor_layer);
            assert_rnp_success(rnp_output_armor_set_line_length(armor_layer, llen));

            // export
            assert_rnp_success(rnp_key_export(key, armor_layer, RNP_KEY_EXPORT_SECRET));
            assert_rnp_success(rnp_output_finish(armor_layer));

            // get output
            buf = NULL;
            assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
            assert_non_null(buf);
            std::string data = std::string(buf, buf + buf_len);
            auto        effective_llen = get_longest_line_length(data, lines_to_skip);
            assert_int_equal(llen / 4, effective_llen / 4);
            assert_true(llen >= effective_llen);

            // check results
            check_loaded_keys("GPG", true, buf, buf_len, "keyid", {"2FCADF05FFA501BB"}, true);

            // cleanup
            rnp_output_destroy(armor_layer);
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
            assert_rnp_success(rnp_output_to_armor(output, &armor_layer, "public key"));
            assert_non_null(armor_layer);
            assert_rnp_success(rnp_output_armor_set_line_length(armor_layer, llen));

            // export
            assert_rnp_success(rnp_key_export(key, armor_layer, RNP_KEY_EXPORT_PUBLIC));
            assert_rnp_success(rnp_output_finish(armor_layer));

            // get output
            buf = NULL;
            assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
            assert_non_null(buf);
            std::string data = std::string(buf, buf + buf_len);
            auto        effective_llen = get_longest_line_length(data, lines_to_skip);
            assert_int_equal(llen / 4, effective_llen / 4);
            assert_true(llen >= effective_llen);

            // check results
            check_loaded_keys("GPG",
                              true,
                              buf,
                              buf_len,
                              "keyid",
                              {"2FCADF05FFA501BB", "54505A936A4A970E"},
                              false);

            // cleanup
            rnp_output_destroy(armor_layer);
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
            assert_rnp_success(rnp_output_to_armor(output, &armor_layer, "secret key"));
            assert_non_null(armor_layer);
            assert_rnp_success(rnp_output_armor_set_line_length(armor_layer, llen));

            // export
            assert_rnp_success(rnp_key_export(key, armor_layer, RNP_KEY_EXPORT_SECRET));
            assert_rnp_success(rnp_output_finish(armor_layer));

            // get output
            buf = NULL;
            assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_len, false));
            assert_non_null(buf);
            std::string data = std::string(buf, buf + buf_len);
            auto        effective_llen = get_longest_line_length(data, lines_to_skip);
            assert_int_equal(llen / 4, effective_llen / 4);
            assert_true(llen >= effective_llen);

            // check results
            check_loaded_keys("GPG",
                              true,
                              buf,
                              buf_len,
                              "keyid",
                              {"2FCADF05FFA501BB", "54505A936A4A970E"},
                              true);

            // cleanup
            rnp_output_destroy(armor_layer);
            rnp_output_destroy(output);
            rnp_key_handle_destroy(key);
        }
    }
    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_dump)
{
    rnp_ffi_t        ffi = NULL;
    rnp_key_handle_t key = NULL;
    char *           json = NULL;
    json_object *    jso = NULL;

    // setup FFI
    test_ffi_init(&ffi);

    // locate key
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2FCADF05FFA501BB", &key));
    assert_non_null(key);

    // dump public key and check results
    assert_rnp_success(rnp_key_packets_to_json(
      key, false, RNP_JSON_DUMP_MPI | RNP_JSON_DUMP_RAW | RNP_JSON_DUMP_GRIP, &json));
    assert_non_null(json);
    jso = json_tokener_parse(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_array));
    json_object_put(jso);
    rnp_buffer_destroy(json);

    // dump secret key and check results
    assert_rnp_success(rnp_key_packets_to_json(
      key, true, RNP_JSON_DUMP_MPI | RNP_JSON_DUMP_RAW | RNP_JSON_DUMP_GRIP, &json));
    assert_non_null(json);
    jso = json_tokener_parse(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_array));
    json_object_put(jso);
    rnp_buffer_destroy(json);

    // cleanup
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_dump_edge_cases)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* secret key, stored on gpg card, with too large card serial len */
    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-2-card-len.pgp"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dump_packets_to_output(input, output, 0));
    rnp_input_destroy(input);
    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    std::string dstr(buf, buf + len);
    assert_true(
      dstr.find("card serial number: 0x000102030405060708090a0b0c0d0e0f (16 bytes)") !=
      std::string::npos);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-2-card-len.pgp"));
    char *json = NULL;
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    rnp_input_destroy(input);
    dstr = json;
    assert_true(dstr.find("\"card serial number\":\"000102030405060708090a0b0c0d0e0f\"") !=
                std::string::npos);
    rnp_buffer_destroy(json);

    /* secret key, stored with unknown gpg s2k */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-3.pgp"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dump_packets_to_output(input, output, 0));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    dstr = std::string(buf, buf + len);
    assert_true(dstr.find("Unknown experimental s2k: 0x474e5503 (4 bytes)") !=
                std::string::npos);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-3.pgp"));
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    rnp_input_destroy(input);
    dstr = json;
    assert_true(dstr.find("\"unknown experimental\":\"474e5503\"") != std::string::npos);
    rnp_buffer_destroy(json);

    /* secret key, stored with unknown s2k */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-unknown.pgp"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dump_packets_to_output(input, output, 0));
    rnp_input_destroy(input);
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    dstr = std::string(buf, buf + len);
    assert_true(dstr.find("Unknown experimental s2k: 0x554e4b4e (4 bytes)") !=
                std::string::npos);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-s2k-101-unknown.pgp"));
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    rnp_input_destroy(input);
    dstr = json;
    assert_true(dstr.find("\"unknown experimental\":\"554e4b4e\"") != std::string::npos);
    rnp_buffer_destroy(json);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_userid_dump_has_no_special_chars)
{
    rnp_ffi_t    ffi = NULL;
    char *       json = NULL;
    json_object *jso = NULL;
    const char * trackers[] = {
      "userid\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f@rnp",
      "userid\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f@rnp"};
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    for (int i = 0; i < 2; i++) {
        // generate RSA key
        rnp_op_generate_t keygen = NULL;
        assert_rnp_success(rnp_op_generate_create(&keygen, ffi, "RSA"));
        assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
        // user id
        assert_rnp_success(rnp_op_generate_set_userid(keygen, trackers[0]));
        // now execute keygen operation
        assert_rnp_success(rnp_op_generate_execute(keygen));
        rnp_key_handle_t key = NULL;
        assert_rnp_success(rnp_op_generate_get_key(keygen, &key));
        assert_non_null(key);
        assert_rnp_success(rnp_op_generate_destroy(keygen));
        keygen = NULL;

        // dump public key and check results
        assert_rnp_success(rnp_key_packets_to_json(
          key, false, RNP_JSON_DUMP_MPI | RNP_JSON_DUMP_RAW | RNP_JSON_DUMP_GRIP, &json));
        assert_non_null(json);
        for (char c = 1; c < 0x20; c++) {
            if (c != '\n') {
                assert_null(strchr(json, c));
            }
        }
        jso = json_tokener_parse(json);
        assert_non_null(jso);
        assert_true(json_object_is_type(jso, json_type_array));
        json_object_put(jso);
        rnp_buffer_destroy(json);

        // cleanup
        rnp_key_handle_destroy(key);
    }
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_pkt_dump)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    char *       json = NULL;
    json_object *jso = NULL;

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    // setup input
    assert_rnp_success(rnp_input_from_path(&input, "data/keyrings/1/pubring.gpg"));

    // try with wrong parameters
    assert_rnp_failure(rnp_dump_packets_to_json(input, 0, NULL));
    assert_rnp_failure(rnp_dump_packets_to_json(NULL, 0, &json));
    assert_rnp_failure(rnp_dump_packets_to_json(input, 117, &json));
    // dump
    assert_rnp_success(rnp_dump_packets_to_json(
      input, RNP_JSON_DUMP_MPI | RNP_JSON_DUMP_RAW | RNP_JSON_DUMP_GRIP, &json));
    rnp_input_destroy(input);
    input = NULL;
    assert_non_null(json);

    // check results
    jso = json_tokener_parse(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_array));
    /* make sure that correct number of packets dumped */
    assert_int_equal(json_object_array_length(jso), 35);
    json_object_put(jso);
    rnp_buffer_destroy(json);

    // setup input and output
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_input_from_path(&input, "data/keyrings/1/pubring.gpg"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));

    // try with wrong parameters
    assert_rnp_failure(rnp_dump_packets_to_output(input, NULL, 0));
    assert_rnp_failure(rnp_dump_packets_to_output(NULL, output, 0));
    assert_rnp_failure(rnp_dump_packets_to_output(input, output, 117));
    // dump
    assert_rnp_success(
      rnp_dump_packets_to_output(input, output, RNP_DUMP_MPI | RNP_DUMP_RAW | RNP_DUMP_GRIP));

    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    /* make sure output is not cut */
    assert_true(len > 45000);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    // dump data with marker packet
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.marker"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(
      rnp_dump_packets_to_output(input, output, RNP_DUMP_MPI | RNP_DUMP_RAW | RNP_DUMP_GRIP));
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    buf[len - 1] = '\0';
    assert_non_null(strstr((char *) buf, "contents: PGP"));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    // dump data with marker packet to json
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.marker"));
    assert_rnp_success(rnp_dump_packets_to_json(
      input, RNP_JSON_DUMP_MPI | RNP_JSON_DUMP_RAW | RNP_JSON_DUMP_GRIP, &json));
    assert_non_null(strstr(json, "\"contents\":\"PGP\""));
    rnp_buffer_destroy(json);
    rnp_input_destroy(input);

    // dump data with malformed marker packet
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.marker.malf"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(
      rnp_dump_packets_to_output(input, output, RNP_DUMP_MPI | RNP_DUMP_RAW | RNP_DUMP_GRIP));
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    buf[len - 1] = '\0';
    assert_non_null(strstr((char *) buf, "contents: invalid"));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    // dump data with malformed marker packet to json
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.marker.malf"));
    assert_rnp_success(rnp_dump_packets_to_json(
      input, RNP_JSON_DUMP_MPI | RNP_JSON_DUMP_RAW | RNP_JSON_DUMP_GRIP, &json));
    assert_non_null(strstr(json, "\"contents\":\"invalid\""));
    rnp_buffer_destroy(json);
    rnp_input_destroy(input);

    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_rsa_v3_dump)
{
    rnp_input_t input = NULL;
    char *      json = NULL;

    /* dump rsav3 key to json via FFI */
    assert_rnp_success(rnp_input_from_path(&input, "data/keyrings/4/rsav3-p.asc"));
    assert_rnp_success(rnp_dump_packets_to_json(input, RNP_JSON_DUMP_GRIP, &json));
    rnp_input_destroy(input);
    /* parse dump */
    json_object *jso = json_tokener_parse(json);
    rnp_buffer_destroy(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_array));
    json_object *rsapkt = json_object_array_get_idx(jso, 0);
    assert_non_null(rsapkt);
    assert_true(json_object_is_type(rsapkt, json_type_object));
    /* check algorithm string */
    json_object *fld = NULL;
    assert_true(json_object_object_get_ex(rsapkt, "algorithm.str", &fld));
    assert_non_null(fld);
    const char *str = json_object_get_string(fld);
    assert_non_null(str);
    assert_string_equal(str, "RSA (Encrypt or Sign)");
    /* check fingerprint */
    fld = NULL;
    assert_true(json_object_object_get_ex(rsapkt, "fingerprint", &fld));
    assert_non_null(fld);
    str = json_object_get_string(fld);
    assert_non_null(str);
    assert_string_equal(str, "06a044022bb5aa7991077466aeba2ce7");
    json_object_put(jso);
}

TEST_F(rnp_tests, test_ffi_load_userattr)
{
    rnp_ffi_t ffi = NULL;

    // init ffi and load key
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/ecc-25519-photo-pub.asc"));
    // check userid 0 : ecc-25519
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "cc786278981b0728", &key));
    assert_non_null(key);
    size_t uid_count = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &uid_count));
    assert_int_equal(uid_count, 2);
    char *uid = NULL;
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uid));
    assert_string_equal(uid, "ecc-25519");
    rnp_buffer_destroy(uid);
    // check userattr 1, must be text instead of binary JPEG data
    assert_rnp_success(rnp_key_get_uid_at(key, 1, &uid));
    assert_string_equal(uid, "(photo)");
    rnp_buffer_destroy(uid);
    assert_rnp_success(rnp_key_handle_destroy(key));
    // cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_revocations)
{
    rnp_ffi_t ffi = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load key with revoked userid
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/ecc-p256-revoked-uid.asc"));
    // check userid 0 : ecc-p256
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_non_null(key);
    size_t uid_count = 0;
    assert_rnp_success(rnp_key_get_uid_count(key, &uid_count));
    assert_int_equal(uid_count, 2);
    char *uid = NULL;
    assert_rnp_success(rnp_key_get_uid_at(key, 0, &uid));
    assert_string_equal(uid, "ecc-p256");
    rnp_buffer_destroy(uid);
    rnp_uid_handle_t uid_handle = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid_handle));
    assert_non_null(uid_handle);
    bool revoked = true;
    assert_rnp_failure(rnp_uid_is_revoked(NULL, &revoked));
    assert_rnp_failure(rnp_uid_is_revoked(uid_handle, NULL));
    assert_rnp_success(rnp_uid_is_revoked(uid_handle, &revoked));
    assert_false(revoked);
    const uintptr_t        p_sig = 0xdeadbeef;
    rnp_signature_handle_t sig = reinterpret_cast<rnp_signature_handle_t>(p_sig);
    assert_rnp_failure(rnp_uid_get_revocation_signature(NULL, &sig));
    assert_rnp_failure(rnp_uid_get_revocation_signature(uid_handle, NULL));
    assert_rnp_success(rnp_uid_get_revocation_signature(uid_handle, &sig));
    assert_null(sig);
    assert_rnp_success(rnp_uid_handle_destroy(uid_handle));
    // check userid 1: ecc-p256-revoked
    assert_rnp_success(rnp_key_get_uid_at(key, 1, &uid));
    assert_string_equal(uid, "ecc-p256-revoked");
    rnp_buffer_destroy(uid);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 1, &uid_handle));
    assert_non_null(uid_handle);
    assert_rnp_success(rnp_uid_is_revoked(uid_handle, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_uid_get_revocation_signature(uid_handle, &sig));
    assert_non_null(sig);
    uint32_t creation = 0;
    assert_rnp_success(rnp_signature_get_creation(sig, &creation));
    assert_int_equal(creation, 1556630215);
    assert_rnp_success(rnp_signature_handle_destroy(sig));
    assert_rnp_success(rnp_uid_handle_destroy(uid_handle));
    assert_rnp_success(rnp_key_handle_destroy(key));

    // load key with revoked subkey
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/ecc-p256-revoked-sub.asc"));
    // key is not revoked
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_rnp_success(rnp_key_is_revoked(key, &revoked));
    assert_false(revoked);
    assert_rnp_failure(rnp_key_get_revocation_signature(NULL, &sig));
    assert_rnp_failure(rnp_key_get_revocation_signature(key, NULL));
    assert_rnp_success(rnp_key_get_revocation_signature(key, &sig));
    assert_null(sig);
    bool valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    uint32_t till = 0;
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, 0xFFFFFFFF);
    assert_rnp_success(rnp_key_handle_destroy(key));
    // subkey is revoked
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "37E285E9E9851491", &key));
    assert_rnp_success(rnp_key_is_revoked(key, &revoked));
    assert_true(revoked);
    char *reason = NULL;
    assert_rnp_success(rnp_key_get_revocation_reason(key, &reason));
    assert_string_equal(reason, "Subkey revocation test.");
    rnp_buffer_destroy(reason);
    assert_rnp_success(rnp_key_is_superseded(key, &revoked));
    assert_false(revoked);
    assert_rnp_success(rnp_key_is_compromised(key, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_is_retired(key, &revoked));
    assert_false(revoked);
    assert_rnp_success(rnp_key_get_revocation_signature(key, &sig));
    assert_non_null(sig);
    assert_rnp_success(rnp_signature_get_creation(sig, &creation));
    assert_int_equal(creation, 1556630749);
    assert_rnp_success(rnp_signature_handle_destroy(sig));
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_false(valid);
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, 0);
    assert_rnp_success(rnp_key_handle_destroy(key));

    // load revoked key
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/ecc-p256-revoked-key.asc"));
    // key is revoked
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_rnp_success(rnp_key_is_revoked(key, &revoked));
    assert_true(revoked);
    reason = NULL;
    assert_rnp_success(rnp_key_get_revocation_reason(key, &reason));
    assert_string_equal(reason, "Superseded key test.");
    rnp_buffer_destroy(reason);
    assert_rnp_success(rnp_key_is_superseded(key, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_is_compromised(key, &revoked));
    assert_false(revoked);
    assert_rnp_success(rnp_key_is_retired(key, &revoked));
    assert_false(revoked);
    assert_rnp_success(rnp_key_get_revocation_signature(key, &sig));
    assert_non_null(sig);
    assert_rnp_success(rnp_signature_get_creation(sig, &creation));
    assert_int_equal(creation, 1556799806);
    assert_rnp_success(rnp_signature_handle_destroy(sig));
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_false(valid);
    assert_rnp_success(rnp_key_valid_till(key, &till));
    assert_int_equal(till, 1556799806);
    uint64_t till64 = 0;
    assert_rnp_success(rnp_key_valid_till64(key, &till64));
    assert_int_equal(till64, 1556799806);
    assert_rnp_success(rnp_key_handle_destroy(key));

    // cleanup
    rnp_ffi_destroy(ffi);
}

#define KEY_OUT_PATH "exported-key.asc"

TEST_F(rnp_tests, test_ffi_file_output)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load two keys
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/ecc-p256-pub.asc"));
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/ecc-p521-pub.asc"));

    rnp_key_handle_t k256 = NULL;
    rnp_key_handle_t k521 = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &k256));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p521", &k521));

    rnp_output_t output = NULL;
    // test output to path - must overwrite if exists
    assert_rnp_success(rnp_output_to_path(&output, KEY_OUT_PATH));
    assert_rnp_success(rnp_key_export(
      k256, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_ARMORED | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_output_destroy(output));
    assert_true(rnp_file_exists(KEY_OUT_PATH));
    off_t sz = file_size(KEY_OUT_PATH);
    assert_rnp_success(rnp_output_to_path(&output, KEY_OUT_PATH));
    assert_rnp_success(rnp_key_export(
      k521, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_ARMORED | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_output_destroy(output));
    assert_true(rnp_file_exists(KEY_OUT_PATH));
    assert_true(sz != file_size(KEY_OUT_PATH));
    sz = file_size(KEY_OUT_PATH);
    // test output to file - will fail without overwrite
    assert_rnp_failure(rnp_output_to_file(NULL, KEY_OUT_PATH, RNP_OUTPUT_FILE_OVERWRITE));
    assert_rnp_failure(rnp_output_to_file(&output, NULL, RNP_OUTPUT_FILE_OVERWRITE));
    assert_rnp_failure(rnp_output_to_file(&output, KEY_OUT_PATH, 0));
    // fail with wrong flags
    assert_rnp_failure(rnp_output_to_file(&output, KEY_OUT_PATH, 0x100));
    // test output to random file - will succeed on creation and export but fail on finish.
    assert_rnp_success(rnp_output_to_file(&output, KEY_OUT_PATH, RNP_OUTPUT_FILE_RANDOM));
    assert_true(file_size(KEY_OUT_PATH) == sz);
    assert_rnp_success(
      rnp_key_export(k256, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_failure(rnp_output_finish(output));
    assert_rnp_success(rnp_output_destroy(output));
    // test output with random + overwrite - will succeed
    assert_rnp_success(rnp_output_to_file(
      &output, KEY_OUT_PATH, RNP_OUTPUT_FILE_RANDOM | RNP_OUTPUT_FILE_OVERWRITE));
    assert_true(file_size(KEY_OUT_PATH) == sz);
    assert_rnp_success(
      rnp_key_export(k256, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_output_finish(output));
    assert_rnp_success(rnp_output_destroy(output));
    assert_true(file_size(KEY_OUT_PATH) != sz);
    sz = file_size(KEY_OUT_PATH);
    // test output with just overwrite - will succeed
    assert_rnp_success(rnp_output_to_file(&output, KEY_OUT_PATH, RNP_OUTPUT_FILE_OVERWRITE));
    assert_true(file_size(KEY_OUT_PATH) == 0);
    assert_rnp_success(
      rnp_key_export(k521, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_output_finish(output));
    assert_rnp_success(rnp_output_destroy(output));
    assert_true(file_size(KEY_OUT_PATH) != sz);
    assert_int_equal(rnp_unlink(KEY_OUT_PATH), 0);
    // cleanup
    assert_rnp_success(rnp_key_handle_destroy(k256));
    assert_rnp_success(rnp_key_handle_destroy(k521));
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_stdout_output)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/ecc-p256-pub.asc"));

    rnp_key_handle_t k256 = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &k256));

    rnp_output_t output = NULL;
    assert_rnp_failure(rnp_output_to_stdout(NULL));
    assert_rnp_success(rnp_output_to_stdout(&output));
    assert_rnp_success(rnp_key_export(
      k256, output, RNP_KEY_EXPORT_PUBLIC | RNP_KEY_EXPORT_ARMORED | RNP_KEY_EXPORT_SUBKEYS));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_key_handle_destroy(k256));
    rnp_ffi_destroy(ffi);
}

/* shrink the length to 1 packet
 * set packet length type as PGP_PTAG_OLD_LEN_1 and remove one octet from length header
 */
static std::vector<uint8_t>
shrink_len_2_to_1(const std::vector<uint8_t> &src)
{
    // make sure the most significant octet of 2-octet length is actually zero
    if ((src.size() < 3) || (src[1] != 0)) {
        throw std::invalid_argument("src");
    }

    std::vector<uint8_t> dst;
    dst.reserve(src.size() - 1);
    dst.push_back(PGP_PTAG_ALWAYS_SET | (PGP_PKT_PUBLIC_KEY << PGP_PTAG_OF_CONTENT_TAG_SHIFT) |
                  PGP_PTAG_OLD_LEN_1);
    dst.push_back(src[2]);
    dst.insert(dst.end(), src.begin() + 3, src.end());
    return dst;
}

/*
 * fake a packet with len = 0xEEEE
 */
static std::vector<uint8_t>
fake_len_EEEE(const std::vector<uint8_t> &src)
{
    std::vector<uint8_t> dst = std::vector<uint8_t>(src);
    dst[1] = 0xEE;
    dst[2] = 0xEE;
    return dst;
}

/*
 * fake a packet with len = 0x00
 */
static std::vector<uint8_t>
fake_len_0(const std::vector<uint8_t> &src)
{
    std::vector<uint8_t> dst = shrink_len_2_to_1(src);
    // erase subsequent octets for the packet to correspond the length
    uint8_t old_length = dst[1];
    dst.erase(dst.begin() + 2, dst.begin() + 2 + old_length);
    dst[1] = 0;
    return dst;
}

/* extend the length to 4 octets (preserving the value)
 * set packet length type as PGP_PTAG_OLD_LEN_4 and set 4 octet length instead of 2
 */
static std::vector<uint8_t>
extend_len_2_to_4(const std::vector<uint8_t> &src)
{
    std::vector<uint8_t> dst = std::vector<uint8_t>();
    dst.reserve(src.size() + 2);
    dst.insert(dst.end(), src.begin(), src.begin() + 3);
    dst[0] &= ~PGP_PTAG_OF_LENGTH_TYPE_MASK;
    dst[0] |= PGP_PTAG_OLD_LEN_4;
    dst.insert(dst.begin() + 1, 2, 0);
    dst.insert(dst.end(), src.begin() + 3, src.end());
    return dst;
}

static bool
import_public_keys_from_vector(std::vector<uint8_t> keyring)
{
    rnp_ffi_t ffi = NULL;
    rnp_ffi_create(&ffi, "GPG", "GPG");
    bool res = import_pub_keys(ffi, &keyring[0], keyring.size());
    rnp_ffi_destroy(ffi);
    return res;
}

TEST_F(rnp_tests, test_ffi_import_keys_check_pktlen)
{
    std::vector<uint8_t> keyring = file_to_vec("data/keyrings/2/pubring.gpg");
    // check tag
    // we are assuming that original key uses old format and packet length type is
    // PGP_PTAG_OLD_LEN_2
    assert_true(keyring.size() >= 5);
    uint8_t expected_tag = PGP_PTAG_ALWAYS_SET |
                           (PGP_PKT_PUBLIC_KEY << PGP_PTAG_OF_CONTENT_TAG_SHIFT) |
                           PGP_PTAG_OLD_LEN_2;
    assert_int_equal(expected_tag, 0x99);
    assert_int_equal(keyring[0], expected_tag);
    // original file can be loaded correctly
    assert_true(import_public_keys_from_vector(keyring));
    {
        // Shrink the packet length to 1 octet
        std::vector<uint8_t> keyring_valid_1 = shrink_len_2_to_1(keyring);
        assert_int_equal(keyring_valid_1.size(), keyring.size() - 1);
        assert_true(import_public_keys_from_vector(keyring_valid_1));
    }
    {
        // get invalid key with length 0
        std::vector<uint8_t> keyring_invalid_0 = fake_len_0(keyring);
        assert_false(import_public_keys_from_vector(keyring_invalid_0));
    }
    {
        // get invalid key with length 0xEEEE
        std::vector<uint8_t> keyring_invalid_EEEE = fake_len_EEEE(keyring);
        assert_int_equal(keyring_invalid_EEEE.size(), keyring.size());
        assert_false(import_public_keys_from_vector(keyring_invalid_EEEE));
    }
    {
        std::vector<uint8_t> keyring_len_4 = extend_len_2_to_4(keyring);
        assert_int_equal(keyring_len_4.size(), keyring.size() + 2);
        assert_true(import_public_keys_from_vector(keyring_len_4));
        // get invalid key with length 0xEEEEEEEE
        keyring_len_4[1] = 0xEE;
        keyring_len_4[2] = 0xEE;
        keyring_len_4[3] = 0xEE;
        keyring_len_4[4] = 0xEE;
        assert_false(import_public_keys_from_vector(keyring_len_4));
    }
}

TEST_F(rnp_tests, test_ffi_calculate_iterations)
{
    size_t iterations = 0;
    assert_rnp_failure(rnp_calculate_iterations(NULL, 500, &iterations));
    assert_rnp_failure(rnp_calculate_iterations("SHA256", 500, NULL));
    assert_rnp_failure(rnp_calculate_iterations("WRONG", 500, &iterations));
    assert_rnp_success(rnp_calculate_iterations("SHA256", 500, &iterations));
    assert_true(iterations > 65536);
}

static bool
check_features(const char *type, const char *json, size_t count)
{
    size_t got_count = 0;

    json_object *features = json_tokener_parse(json);
    if (!features) {
        return false;
    }
    bool res = false;
    if (!json_object_is_type(features, json_type_array)) {
        goto done;
    }
    got_count = json_object_array_length(features);
    if (got_count != count) {
        RNP_LOG("wrong feature count for %s: expected %zu, got %zu", type, count, got_count);
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        json_object *val = json_object_array_get_idx(features, i);
        const char * str = json_object_get_string(val);
        bool         supported = false;
        if (!str || rnp_supports_feature(type, str, &supported) || !supported) {
            goto done;
        }
    }

    res = true;
done:
    json_object_put(features);
    return res;
}

TEST_F(rnp_tests, test_ffi_supported_features)
{
    char *features = NULL;
    /* some edge cases */
    assert_rnp_failure(rnp_supported_features(NULL, &features));
    assert_rnp_failure(rnp_supported_features("something", NULL));
    assert_rnp_failure(rnp_supported_features(RNP_FEATURE_SYMM_ALG, NULL));
    assert_rnp_failure(rnp_supported_features("something", &features));
    /* symmetric algorithms */
    assert_rnp_success(rnp_supported_features("Symmetric Algorithm", &features));
    assert_non_null(features);
    bool has_sm2 = sm2_enabled();
    bool has_tf = twofish_enabled();
    bool has_brainpool = brainpool_enabled();
    bool has_idea = idea_enabled();
    assert_true(
      check_features(RNP_FEATURE_SYMM_ALG,
                     features,
                     7 + has_sm2 + has_tf + has_idea + blowfish_enabled() + cast5_enabled()));
    rnp_buffer_destroy(features);
    bool supported = false;
    assert_rnp_failure(rnp_supports_feature(NULL, "IDEA", &supported));
    assert_rnp_failure(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, NULL, &supported));
    assert_rnp_failure(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "IDEA", NULL));
    assert_rnp_failure(rnp_supports_feature("WRONG", "IDEA", &supported));
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "IDEA", &supported));
    assert_true(supported == has_idea);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "TRIPLEDES", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "CAST5", &supported));
    assert_int_equal(supported, cast5_enabled());
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "BLOWFISH", &supported));
    assert_int_equal(supported, blowfish_enabled());
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "AES128", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "AES192", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "AES256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "TWOFISH", &supported));
    assert_true(supported == has_tf);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "CAMELLIA128", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "CAMELLIA192", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "CAMELLIA256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "SM4", &supported));
    assert_true(supported == has_sm2);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "idea", &supported));
    assert_true(supported == has_idea);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "tripledes", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "cast5", &supported));
    assert_true(supported == cast5_enabled());
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "blowfish", &supported));
    assert_true(supported == blowfish_enabled());
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "aes128", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "aes192", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "aes256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "twofish", &supported));
    assert_true(supported == has_tf);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "camellia128", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "camellia192", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "camellia256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "sm4", &supported));
    assert_true(supported == has_sm2);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_SYMM_ALG, "wrong", &supported));
    assert_false(supported);
    /* aead algorithms */
    bool has_eax = aead_eax_enabled();
    bool has_ocb = aead_ocb_enabled();
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_AEAD_ALG, &features));
    assert_non_null(features);
    assert_true(check_features(RNP_FEATURE_AEAD_ALG, features, 1 + has_eax + has_ocb));
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "eax", &supported));
    assert_true(supported == has_eax);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "ocb", &supported));
    assert_true(supported == has_ocb);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "none", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_AEAD_ALG, "wrong", &supported));
    assert_false(supported);
    /* protection mode */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_PROT_MODE, &features));
    assert_non_null(features);
    assert_true(check_features(RNP_FEATURE_PROT_MODE, features, 1));
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PROT_MODE, "cfb", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PROT_MODE, "wrong", &supported));
    assert_false(supported);
    /* public key algorithm */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_PK_ALG, &features));
    assert_non_null(features);
    size_t pqc_opt = 0;
    size_t crypto_refresh_opt = 0;
#if defined(ENABLE_CRYPTO_REFRESH)
    crypto_refresh_opt = 2; // X25519 + ED25519
#endif
#if defined(ENABLE_PQC)
    pqc_opt = 12; // kyber+ecc and dilithium+ecc and sphincs+ variants
#endif
    assert_true(check_features(
      RNP_FEATURE_PK_ALG, features, 6 + has_sm2 + pqc_opt + crypto_refresh_opt));
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "RSA", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "DSA", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "ELGAMAL", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "ECDSA", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "ECDH", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "EDDSA", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "SM2", &supported));
    assert_true(supported == has_sm2);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "rsa", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "dsa", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "elgamal", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "ecdsa", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "ecdh", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "eddsa", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "sm2", &supported));
    assert_true(supported == has_sm2);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_PK_ALG, "wrong", &supported));
    assert_false(supported);
    /* hash algorithm */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_HASH_ALG, &features));
    assert_non_null(features);
    assert_true(
      check_features(RNP_FEATURE_HASH_ALG, features, 8 + has_sm2 + ripemd160_enabled()));
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "MD5", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SHA1", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "RIPEMD160", &supported));
    assert_true(supported == ripemd160_enabled());
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SHA256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SHA384", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SHA512", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SHA224", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SHA3-256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SHA3-512", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "SM3", &supported));
    assert_true(supported == has_sm2);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "md5", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "sha1", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "ripemd160", &supported));
    assert_true(supported == ripemd160_enabled());
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "sha256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "sha384", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "sha512", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "sha224", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "sha3-256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "sha3-512", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "sm3", &supported));
    assert_true(supported == has_sm2);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "wrong", &supported));
    assert_false(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_HASH_ALG, "CRC24", &supported));
    assert_false(supported);
    /* compression algorithm */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_COMP_ALG, &features));
    assert_non_null(features);
    assert_true(check_features(RNP_FEATURE_COMP_ALG, features, 4));
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_COMP_ALG, "Uncompressed", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_COMP_ALG, "Zlib", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_COMP_ALG, "ZIP", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_COMP_ALG, "BZIP2", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_COMP_ALG, "wrong", &supported));
    assert_false(supported);
    /* elliptic curve */
    assert_rnp_success(rnp_supported_features(RNP_FEATURE_CURVE, &features));
    assert_non_null(features);
    assert_true(check_features(RNP_FEATURE_CURVE, features, 6 + has_sm2 + 3 * has_brainpool));
    rnp_buffer_destroy(features);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "NIST P-256", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "NIST P-384", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "NIST P-521", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "ed25519", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "curve25519", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP256r1", &supported));
    assert_true(supported == has_brainpool);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP384r1", &supported));
    assert_true(supported == has_brainpool);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "brainpoolP512r1", &supported));
    assert_true(supported == has_brainpool);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "secp256k1", &supported));
    assert_true(supported);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "SM2 P-256", &supported));
    assert_true(supported == has_sm2);
    assert_rnp_success(rnp_supports_feature(RNP_FEATURE_CURVE, "wrong", &supported));
    assert_false(supported);
}

TEST_F(rnp_tests, test_ffi_output_to_armor)
{
    rnp_ffi_t    ffi = NULL;
    rnp_output_t memory = NULL;
    rnp_output_t armor = NULL;
    rnp_input_t  input = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2FCADF05FFA501BB", &key));
    assert_non_null(key);

    assert_rnp_success(rnp_output_to_memory(&memory, 0));
    /* some edge cases */
    assert_rnp_failure(rnp_output_to_armor(NULL, &armor, "message"));
    assert_null(armor);
    assert_rnp_failure(rnp_output_to_armor(memory, NULL, "message"));
    assert_null(armor);
    assert_rnp_failure(rnp_output_to_armor(memory, &armor, "wrong"));
    assert_null(armor);
    /* export raw key to armored stream with 'message' header */
    assert_rnp_success(rnp_output_to_armor(memory, &armor, "message"));
    assert_rnp_success(rnp_key_export(key, armor, RNP_KEY_EXPORT_PUBLIC));
    assert_rnp_success(rnp_output_destroy(armor));
    uint8_t *buf = NULL;
    size_t   buf_len = 0;
    /* check contents to make sure it is correct armored stream */
    assert_rnp_success(rnp_output_memory_get_buf(memory, &buf, &buf_len, false));
    assert_non_null(buf);
    const char *hdr = "-----BEGIN PGP MESSAGE-----";
    assert_true(buf_len > strlen(hdr));
    assert_int_equal(strncmp((char *) buf, hdr, strlen(hdr)), 0);
    assert_rnp_success(rnp_input_from_memory(&input, buf, buf_len, false));
    rnp_output_t memory2 = NULL;
    assert_rnp_success(rnp_output_to_memory(&memory2, 0));
    assert_rnp_success(rnp_dearmor(input, memory2));
    rnp_output_destroy(memory2);
    rnp_input_destroy(input);

    rnp_key_handle_destroy(key);
    rnp_output_destroy(memory);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_rnp_guess_contents)
{
    char *      msgt = NULL;
    rnp_input_t input = NULL;
    assert_rnp_failure(rnp_guess_contents(NULL, &msgt));

    assert_rnp_success(
      rnp_input_from_path(&input, "data/issue1188/armored_revocation_signature.pgp"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_int_equal(strcmp(msgt, "signature"), 0);
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_key_merge/key-pub.pgp"));
    assert_rnp_failure(rnp_guess_contents(input, NULL));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "public key");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_merge/key-pub-just-subkey-1.pgp"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "public key");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_key_merge/key-pub.asc"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "public key");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_key_merge/key-sec.pgp"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "secret key");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_key_merge/key-sec.asc"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "secret key");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_key_merge/key-sec-just-subkey-1.pgp"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "secret key");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_z/128mb.zip"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "message");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_z/4gb.bzip2.asc"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "message");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_signatures/source.txt.sig"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "signature");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_signatures/source.txt.sig.asc"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "signature");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_signatures/source.txt.asc.asc"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "cleartext");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_stream_signatures/source.txt"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "unknown");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.marker"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "message");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.wrong-armor.asc"));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "unknown");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    const char *msg1 = "-----BEGIN PGP PGP";
    assert_rnp_success(rnp_input_from_memory(&input, (uint8_t *) msg1, strlen(msg1), false));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "unknown");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);

    const char *msg2 = "-----BEGIN PGP PGP PGP PGP PGP PGP PGP";
    assert_rnp_success(rnp_input_from_memory(&input, (uint8_t *) msg2, strlen(msg2), false));
    assert_rnp_success(rnp_guess_contents(input, &msgt));
    assert_string_equal(msgt, "unknown");
    rnp_buffer_destroy(msgt);
    rnp_input_destroy(input);
}

TEST_F(rnp_tests, test_ffi_literal_filename)
{
    rnp_ffi_t     ffi = NULL;
    rnp_input_t   input = NULL;
    rnp_output_t  output = NULL;
    rnp_op_sign_t op = NULL;
    uint8_t *     signed_buf;
    size_t        signed_len;

    // init ffi
    test_ffi_init(&ffi);
    // init input
    test_ffi_init_sign_memory_input(&input, &output);
    // create signature operation
    assert_rnp_success(rnp_op_sign_create(&op, ffi, input, output));
    // setup signature(s)
    test_ffi_setup_signatures(&ffi, &op);
    // setup filename and modification time
    assert_rnp_failure(rnp_op_sign_set_file_name(NULL, "checkleak.dat"));
    assert_rnp_success(rnp_op_sign_set_file_name(op, "checkleak.dat"));
    assert_rnp_success(rnp_op_sign_set_file_name(op, NULL));
    assert_rnp_success(rnp_op_sign_set_file_name(op, "testfile.dat"));
    assert_rnp_failure(rnp_op_sign_set_file_mtime(NULL, 12345678));
    assert_rnp_success(rnp_op_sign_set_file_mtime(op, 12345678));
    // execute the operation
    assert_rnp_success(rnp_op_sign_execute(op));
    // make sure the output file was created
    assert_rnp_success(rnp_output_memory_get_buf(output, &signed_buf, &signed_len, true));
    assert_non_null(signed_buf);
    assert_true(signed_len > 0);

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_op_sign_destroy(op));
    op = NULL;

    // check the resulting stream for correct name/time
    assert_rnp_success(rnp_input_from_memory(&input, signed_buf, signed_len, false));
    char *json = NULL;
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    assert_non_null(json);

    std::string jstr = json;
    assert_true(jstr.find("\"filename\":\"testfile.dat\"") != std::string::npos);
    assert_true(jstr.find("\"timestamp\":12345678") != std::string::npos);

    assert_rnp_success(rnp_input_destroy(input));
    rnp_buffer_destroy(signed_buf);
    rnp_buffer_destroy(json);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_op_set_hash)
{
    rnp_ffi_t     ffi = NULL;
    rnp_input_t   input = NULL;
    rnp_output_t  output = NULL;
    rnp_op_sign_t op = NULL;
    uint8_t *     signed_buf;
    size_t        signed_len;

    // init ffi
    test_ffi_init(&ffi);
    // init input
    test_ffi_init_sign_memory_input(&input, &output);
    // create signature operation
    assert_rnp_success(rnp_op_sign_create(&op, ffi, input, output));
    // setup signature(s)
    test_ffi_setup_signatures(&ffi, &op);
    // make sure it doesn't fail on NULL hash value
    assert_rnp_failure(rnp_op_sign_set_hash(op, NULL));
    assert_rnp_failure(rnp_op_sign_set_hash(NULL, "SHA256"));
    assert_rnp_failure(rnp_op_sign_set_hash(op, "Unknown"));
    assert_rnp_success(rnp_op_sign_set_hash(op, "SHA256"));
    // execute the operation with wrong password
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "wrong"));
    assert_int_equal(rnp_op_sign_execute(op), RNP_ERROR_BAD_PASSWORD);
    assert_rnp_success(rnp_op_sign_destroy(op));
    // execute the operation with valid password
    assert_rnp_success(rnp_op_sign_create(&op, ffi, input, output));
    // setup signature(s)
    test_ffi_setup_signatures(&ffi, &op);
    assert_rnp_success(rnp_op_sign_execute(op));
    // make sure the output file was created
    assert_rnp_success(rnp_output_memory_get_buf(output, &signed_buf, &signed_len, true));
    assert_non_null(signed_buf);
    assert_true(signed_len > 0);

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_op_sign_destroy(op));

    rnp_buffer_destroy(signed_buf);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_op_set_compression)
{
    rnp_ffi_t     ffi = NULL;
    rnp_input_t   input = NULL;
    rnp_output_t  output = NULL;
    rnp_op_sign_t op = NULL;
    uint8_t *     signed_buf;
    size_t        signed_len;

    // init ffi
    test_ffi_init(&ffi);
    // init input
    test_ffi_init_sign_memory_input(&input, &output);
    // create signature operation
    assert_rnp_success(rnp_op_sign_create(&op, ffi, input, output));
    // setup signature(s)
    test_ffi_setup_signatures(&ffi, &op);
    // make sure it doesn't fail on NULL compression algorithm value
    assert_rnp_failure(rnp_op_sign_set_compression(op, NULL, 6));
    assert_rnp_failure(rnp_op_sign_set_compression(op, "Unknown", 6));
    assert_rnp_failure(rnp_op_sign_set_compression(NULL, "ZLib", 6));
    assert_rnp_success(rnp_op_sign_set_compression(op, "ZLib", 6));
    // execute the operation
    assert_rnp_success(rnp_op_sign_execute(op));
    // make sure the output file was created
    assert_rnp_success(rnp_output_memory_get_buf(output, &signed_buf, &signed_len, true));
    assert_non_null(signed_buf);
    assert_true(signed_len > 0);

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_op_sign_destroy(op));

    rnp_buffer_destroy(signed_buf);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_aead_params)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *plaintext = "Some data to encrypt using the AEAD-EAX and AEAD-OCB encryption.";

    // setup FFI
    test_ffi_init(&ffi);

    // write out some data
    str_to_file("plaintext", plaintext);
    // create input+output
    assert_rnp_success(rnp_input_from_path(&input, "plaintext"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    assert_non_null(output);
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    // setup AEAD params
    assert_rnp_failure(rnp_op_encrypt_set_aead(NULL, "OCB"));
    assert_rnp_failure(rnp_op_encrypt_set_aead(op, NULL));
    assert_rnp_failure(rnp_op_encrypt_set_aead(op, "WRONG"));
    if (!aead_ocb_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_set_aead(op, "OCB"));
    } else {
        assert_rnp_success(rnp_op_encrypt_set_aead(op, "OCB"));
    }
    assert_rnp_failure(rnp_op_encrypt_set_aead_bits(NULL, 10));
    assert_rnp_failure(rnp_op_encrypt_set_aead_bits(op, -1));
    assert_rnp_failure(rnp_op_encrypt_set_aead_bits(op, 60));
    assert_rnp_failure(rnp_op_encrypt_set_aead_bits(op, 17));
    assert_rnp_success(rnp_op_encrypt_set_aead_bits(op, 10));
    // add password (using all defaults)
    assert_rnp_success(rnp_op_encrypt_add_password(op, "pass1", NULL, 0, NULL));
    // setup compression
    assert_rnp_failure(rnp_op_encrypt_set_compression(NULL, "ZLIB", 6));
    assert_rnp_failure(rnp_op_encrypt_set_compression(op, NULL, 6));
    assert_rnp_failure(rnp_op_encrypt_set_compression(op, "WRONG", 6));
    assert_rnp_success(rnp_op_encrypt_set_compression(op, "ZLIB", 6));
    // set filename and mtime
    assert_rnp_failure(rnp_op_encrypt_set_file_name(NULL, "filename"));
    assert_rnp_success(rnp_op_encrypt_set_file_name(op, NULL));
    assert_rnp_success(rnp_op_encrypt_set_file_name(op, "filename"));
    assert_rnp_failure(rnp_op_encrypt_set_file_mtime(NULL, 1000));
    assert_rnp_success(rnp_op_encrypt_set_file_mtime(op, 1000));
    // execute the operation
    assert_rnp_success(rnp_op_encrypt_execute(op));
    // make sure the output file was created
    assert_true(rnp_file_exists("encrypted"));
    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    op = NULL;

    // list packets
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    char *json = NULL;
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    json_object *jso = json_tokener_parse(json);
    rnp_buffer_destroy(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_array));
    /* check the symmetric-key encrypted session key packet */
    json_object *pkt = json_object_array_get_idx(jso, 0);
    assert_true(check_json_pkt_type(pkt, PGP_PKT_SK_SESSION_KEY));
    if (!aead_ocb_enabled()) {
        // if AEAD is not enabled then v4 encrypted packet will be created
        assert_true(check_json_field_int(pkt, "version", 4));
        assert_true(check_json_field_str(pkt, "algorithm.str", "AES-256"));
    } else {
        assert_true(check_json_field_int(pkt, "version", 5));
        assert_true(check_json_field_str(pkt, "aead algorithm.str", "OCB"));
    }
    /* check the aead-encrypted packet */
    pkt = json_object_array_get_idx(jso, 1);
    if (!aead_ocb_enabled()) {
        assert_true(check_json_pkt_type(pkt, PGP_PKT_SE_IP_DATA));
    } else {
        assert_true(check_json_pkt_type(pkt, PGP_PKT_AEAD_ENCRYPTED));
        assert_true(check_json_field_int(pkt, "version", 1));
        assert_true(check_json_field_str(pkt, "aead algorithm.str", "OCB"));
        assert_true(check_json_field_int(pkt, "chunk size", 10));
    }
    json_object_put(jso);

    /* decrypt */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "pass1"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;
    // compare the decrypted file
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    rnp_unlink("decrypted");

    // final cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_detached_verify_input)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    // init ffi
    test_ffi_init(&ffi);
    /* verify detached signature via rnp_op_verify_create - should not crash */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_stream_signatures/source.txt.sig"));
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_detached_cleartext_signed_input)
{
    rnp_ffi_t ffi = NULL;
    test_ffi_init(&ffi);
    /* verify detached signature with cleartext input - must fail */
    rnp_input_t inputmsg = NULL;
    assert_rnp_success(rnp_input_from_path(&inputmsg, "data/test_messages/message.txt"));
    rnp_input_t inputsig = NULL;
    assert_rnp_success(
      rnp_input_from_path(&inputsig, "data/test_messages/message.txt.cleartext-signed"));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, inputmsg, inputsig));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(inputmsg);
    rnp_input_destroy(inputsig);
    /* verify detached signature with signed/embedded input - must fail */
    assert_rnp_success(rnp_input_from_path(&inputmsg, "data/test_messages/message.txt"));
    assert_rnp_success(
      rnp_input_from_path(&inputsig, "data/test_messages/message.txt.empty.sig"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, inputmsg, inputsig));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(inputmsg);
    rnp_input_destroy(inputsig);
    /* verify detached signature as a whole message - must fail */
    assert_rnp_success(rnp_input_from_path(&inputmsg, "data/test_messages/message.txt.sig"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, inputmsg, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    rnp_op_verify_destroy(verify);
    rnp_output_destroy(output);
    rnp_input_destroy(inputmsg);

    rnp_ffi_destroy(ffi);
}

static bool
check_signature(rnp_op_verify_t op, size_t idx, rnp_result_t status)
{
    rnp_op_verify_signature_t sig = NULL;
    if (rnp_op_verify_get_signature_at(op, idx, &sig)) {
        return false;
    }
    return rnp_op_verify_signature_get_status(sig) == status;
}

TEST_F(rnp_tests, test_ffi_op_verify_sig_count)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    // init ffi
    test_ffi_init(&ffi);

    /* signed message */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.signed"));
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_failure(rnp_op_verify_create(NULL, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_create(&verify, NULL, input, output));
    assert_rnp_failure(rnp_op_verify_create(&verify, ffi, NULL, output));
    assert_rnp_failure(rnp_op_verify_create(&verify, ffi, input, NULL));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(NULL));
    assert_rnp_success(rnp_op_verify_execute(verify));
    size_t sigcount = 0;
    assert_rnp_failure(rnp_op_verify_get_signature_count(verify, NULL));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed with unknown key */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.unknown"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_int_equal(rnp_op_verify_execute(verify), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_KEY_NOT_FOUND));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed with malformed signature (bad version) */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.malfsig"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_UNKNOWN));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed with invalid signature (modified hash alg) */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.invsig"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_int_equal(rnp_op_verify_execute(verify), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_INVALID));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed without the signature */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.nosig"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 0);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* detached signature */
    rnp_input_t source = NULL;
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&source, "data/test_messages/message.txt"));
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.sig"));
    assert_rnp_failure(rnp_op_verify_detached_create(NULL, ffi, source, input));
    assert_rnp_failure(rnp_op_verify_detached_create(&verify, NULL, source, input));
    assert_rnp_failure(rnp_op_verify_detached_create(&verify, ffi, NULL, input));
    assert_rnp_failure(rnp_op_verify_detached_create(&verify, ffi, source, NULL));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    /* detached text-mode signature */
    source = NULL;
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&source, "data/test_messages/message.txt"));
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.sig-text"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    source = NULL;
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&source, "data/test_messages/message.txt.crlf"));
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.sig-text"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    char format = 'b';
    assert_rnp_failure(rnp_op_verify_get_format(NULL, &format));
    assert_rnp_failure(rnp_op_verify_get_format(verify, NULL));
    assert_rnp_success(rnp_op_verify_get_format(verify, &format));
    assert_int_equal(format, '\0');
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    /* detached text-mode signature with trailing CR characters */
    source = NULL;
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&source, "data/test_messages/message-trailing-cr.txt"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message-trailing-cr.txt.sig-text"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    /* detached text-mode signature with CRLF on 32k boundary */
    source = NULL;
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&source, "data/test_messages/message-32k-crlf.txt"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message-32k-crlf.txt.sig"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    /* embedded text-mode signature with CRLF on 32k boundary */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message-32k-crlf.txt.gpg"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    format = '\0';
    assert_rnp_failure(rnp_op_verify_get_format(NULL, &format));
    assert_rnp_failure(rnp_op_verify_get_format(verify, NULL));
    assert_rnp_success(rnp_op_verify_get_format(verify, &format));
    assert_int_equal(format, 't');
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* malformed detached signature */
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&source, "data/test_messages/message.txt"));
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.sig.malf"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 0);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    /* malformed detached signature, wrong bitlen in MPI  */
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&source, "data/test_messages/message.txt"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.sig.wrong-mpi-bitlen"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    /* encrypted message */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.encrypted"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    if (!aead_eax_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(verify));
    } else {
        assert_rnp_success(rnp_op_verify_execute(verify));
    }
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 0);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* encrypted and signed message */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-encrypted"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* cleartext signed message */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.cleartext-signed"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* cleartext signed message without newline */
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(
      &input, "data/test_messages/message.txt.cleartext-signed-nonewline"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 0);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* cleartext signed with malformed signature (wrong mpi len) */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.cleartext-malf"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_int_equal(rnp_op_verify_execute(verify), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_UNKNOWN));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* cleartext signed without the signature */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.cleartext-nosig"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 0);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed message without compression */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-no-z"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed and password-encrypted data with 0 compression algo */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-sym-none-z"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed message with one-pass with wrong version */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-no-z-malf"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* encrypted and signed message with marker packet */
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.marker"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* encrypted and signed message with marker packet, armored */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.marker.asc"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* encrypted and signed message with malformed marker packet */
    sigcount = 255;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.marker.malf"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* sha1 detached signature over the collision-suspicious data */
    /* allow sha1 temporary */
    rnp::SecurityRule allow_sha1(
      rnp::FeatureType::Hash, PGP_HASH_SHA1, rnp::SecurityLevel::Default, 1547856001);
    global_ctx.profile.add_rule(allow_sha1);
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&source, "data/test_messages/shattered-1.pdf"));
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/shattered-1.pdf.sig"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_INVALID));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    /* sha1 detached signature over the document with collision*/
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&source, "data/test_messages/shattered-2.pdf"));
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/shattered-1.pdf.sig"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, source, input));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_INVALID));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(source);
    rnp_input_destroy(input);

    /* sha1 attached signature over the collision-suspicious data */
    sigcount = 255;
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/shattered-2.pdf.gpg"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_INVALID));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* remove sha1 rule */
    assert_true(global_ctx.profile.del_rule(allow_sha1));

    /* signed message with key which is now expired */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    import_pub_keys(ffi, "data/test_messages/expired_signing_key-pub.asc");
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "30FC0D776915BA44", &key));
    uint64_t till = 0;
    assert_rnp_success(rnp_key_valid_till64(key, &till));
    assert_int_equal(till, 1623424417);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-expired-key"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed message with subkey which is now expired */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    import_pub_keys(ffi, "data/test_messages/expired_signing_sub-pub.asc");
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "D93A47FD93191FD1", &key));
    till = 0;
    assert_rnp_success(rnp_key_valid_till64(key, &till));
    assert_int_equal(till, 1623933507);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-expired-sub"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed message with md5 hash */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.md5"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_INVALID));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed message with md5 hash before the cut-off date */
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/key-rsa-2001-pub.asc"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-md5-before"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed message with md5 hash right after the cut-off date */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-md5-after"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_INVALID));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* signed message with sha1 hash */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.sha1"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_INVALID));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message signed with sha1 before the cut-off date */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-sha1-before"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_SUCCESS));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message signed with sha1 right after the cut-off date */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-sha1-after"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_true(check_signature(verify, 0, RNP_ERROR_SIGNATURE_INVALID));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message signed with encrypt-only subkey */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    import_pub_keys(ffi, "data/test_messages/key-rsas-rsae.asc");
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message-signed-rsae.txt.pgp"));
    assert_rnp_success(rnp_output_to_null(&output));
    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    rnp_op_verify_signature_t vsig = NULL;
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &vsig));
    assert_int_equal(rnp_op_verify_signature_get_status(vsig), RNP_ERROR_SIGNATURE_INVALID);
    rnp_signature_handle_t sig = NULL;
    assert_rnp_success(rnp_op_verify_signature_get_handle(vsig, &sig));
    char *type = NULL;
    assert_rnp_success(rnp_signature_get_type(sig, &type));
    assert_string_equal(type, "binary");
    rnp_buffer_destroy(type);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    size_t errors = 0;
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 1);
    rnp_result_t error = 0;
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_UNUSABLE_KEY);
    assert_int_equal(rnp_signature_is_valid(sig, RNP_SIGNATURE_REVALIDATE),
                     RNP_ERROR_SIGNATURE_INVALID);
    rnp_signature_handle_destroy(sig);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_op_verify_get_protection_info)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    // init ffi
    test_ffi_init(&ffi);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    /* message just signed */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.signed"));
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    char *mode = NULL;
    char *cipher = NULL;
    bool  valid = true;
    assert_rnp_failure(rnp_op_verify_get_protection_info(NULL, &mode, &cipher, &valid));
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, NULL, NULL));
    assert_string_equal(mode, "none");
    rnp_buffer_destroy(mode);
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, NULL, &cipher, NULL));
    assert_string_equal(cipher, "none");
    rnp_buffer_destroy(cipher);
    valid = true;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, NULL, NULL, &valid));
    assert_false(valid);
    assert_rnp_failure(rnp_op_verify_get_protection_info(verify, NULL, NULL, NULL));
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "none");
    assert_string_equal(cipher, "none");
    assert_false(valid);
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message without MDC */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-no-mdc"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    mode = NULL;
    cipher = NULL;
    valid = true;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "cfb");
    assert_string_equal(cipher, "AES256");
    assert_false(valid);
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message with MDC */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.enc-mdc"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    mode = NULL;
    cipher = NULL;
    valid = false;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "cfb-mdc");
    assert_string_equal(cipher, "AES256");
    assert_true(valid);
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message with AEAD-OCB */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-ocb"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    if (!aead_ocb_enabled() || aead_ocb_aes_only()) {
        assert_rnp_failure(rnp_op_verify_execute(verify));
    } else {
        assert_rnp_success(rnp_op_verify_execute(verify));
    }
    mode = NULL;
    cipher = NULL;
    valid = false;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "aead-ocb");
    assert_string_equal(cipher, "CAMELLIA192");
    assert_true(valid == (aead_ocb_enabled() && !aead_ocb_aes_only()));
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message with AEAD-OCB AES-192 */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-ocb-aes"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    if (!aead_ocb_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(verify));
    } else {
        assert_rnp_success(rnp_op_verify_execute(verify));
    }
    mode = NULL;
    cipher = NULL;
    valid = false;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "aead-ocb");
    assert_string_equal(cipher, "AES192");
    assert_true(valid == aead_ocb_enabled());
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* modified message with AEAD-OCB */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-ocb-malf"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    mode = NULL;
    cipher = NULL;
    valid = false;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "aead-ocb");
    assert_string_equal(cipher, "CAMELLIA192");
    assert_false(valid);
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message with AEAD-EAX */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-eax"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    if (!aead_eax_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(verify));
    } else {
        assert_rnp_success(rnp_op_verify_execute(verify));
    }
    mode = NULL;
    cipher = NULL;
    valid = false;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "aead-eax");
    assert_string_equal(cipher, "AES256");
    assert_true(valid == aead_eax_enabled());
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* modified message with AEAD-EAX */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-eax-malf"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    mode = NULL;
    cipher = NULL;
    valid = false;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "aead-eax");
    assert_string_equal(cipher, "AES256");
    assert_false(valid);
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}

static bool
getpasscb_for_key(rnp_ffi_t        ffi,
                  void *           app_ctx,
                  rnp_key_handle_t key,
                  const char *     pgp_context,
                  char *           buf,
                  size_t           buf_len)
{
    if (!key) {
        return false;
    }
    char *keyid = NULL;
    rnp_key_get_keyid(key, &keyid);
    if (!keyid) {
        return false;
    }
    const char *pass = "password";
    if (strcmp(keyid, (const char *) app_ctx)) {
        pass = "wrongpassword";
    }
    size_t pass_len = strlen(pass);
    rnp_buffer_destroy(keyid);

    if (pass_len >= buf_len) {
        return false;
    }
    memcpy(buf, pass, pass_len + 1);
    return true;
}

TEST_F(rnp_tests, test_ffi_op_verify_recipients_info)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    // init ffi
    test_ffi_init(&ffi);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    /* message just signed */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.signed"));
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    /* check filename and mtime */
    char *   filename = NULL;
    uint32_t mtime = 0;
    assert_rnp_failure(rnp_op_verify_get_file_info(NULL, &filename, &mtime));
    assert_rnp_success(rnp_op_verify_get_file_info(verify, &filename, &mtime));
    assert_string_equal(filename, "message.txt");
    assert_int_equal(mtime, 1571991574);
    rnp_buffer_destroy(filename);
    filename = NULL;
    assert_rnp_success(rnp_op_verify_get_file_info(verify, &filename, NULL));
    assert_string_equal(filename, "message.txt");
    rnp_buffer_destroy(filename);
    mtime = 0;
    assert_rnp_success(rnp_op_verify_get_file_info(verify, NULL, &mtime));
    assert_int_equal(mtime, 1571991574);
    /* rnp_op_verify_get_recipient_count */
    assert_rnp_failure(rnp_op_verify_get_recipient_count(verify, NULL));
    size_t count = 255;
    assert_rnp_failure(rnp_op_verify_get_recipient_count(NULL, &count));
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 0);
    /* rnp_op_verify_get_recipient_at */
    rnp_recipient_handle_t recipient = NULL;
    assert_rnp_failure(rnp_op_verify_get_recipient_at(NULL, 0, &recipient));
    assert_rnp_failure(rnp_op_verify_get_recipient_at(verify, 0, NULL));
    assert_rnp_failure(rnp_op_verify_get_recipient_at(verify, 0, &recipient));
    assert_rnp_failure(rnp_op_verify_get_recipient_at(verify, 10, &recipient));
    /* rnp_op_verify_get_used_recipient */
    assert_rnp_failure(rnp_op_verify_get_used_recipient(NULL, &recipient));
    assert_rnp_failure(rnp_op_verify_get_used_recipient(verify, NULL));
    assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
    assert_null(recipient);
    /* rnp_op_verify_get_symenc_count */
    assert_rnp_failure(rnp_op_verify_get_symenc_count(verify, NULL));
    count = 255;
    assert_rnp_failure(rnp_op_verify_get_symenc_count(NULL, &count));
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 0);
    /* rnp_op_verify_get_symenc_at */
    rnp_symenc_handle_t symenc = NULL;
    assert_rnp_failure(rnp_op_verify_get_symenc_at(NULL, 0, &symenc));
    assert_rnp_failure(rnp_op_verify_get_symenc_at(verify, 0, NULL));
    assert_rnp_failure(rnp_op_verify_get_symenc_at(verify, 0, &symenc));
    assert_rnp_failure(rnp_op_verify_get_symenc_at(verify, 10, &symenc));
    /* rnp_op_verify_get_used_symenc */
    assert_rnp_failure(rnp_op_verify_get_used_symenc(NULL, &symenc));
    assert_rnp_failure(rnp_op_verify_get_used_symenc(verify, NULL));
    assert_rnp_success(rnp_op_verify_get_used_symenc(verify, &symenc));
    assert_null(symenc);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message without MDC: single recipient */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-no-mdc"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 1);
    assert_rnp_failure(rnp_op_verify_get_recipient_at(verify, 1, &recipient));
    assert_rnp_success(rnp_op_verify_get_recipient_at(verify, 0, &recipient));
    assert_non_null(recipient);
    char *alg = NULL;
    assert_rnp_failure(rnp_recipient_get_alg(NULL, &alg));
    assert_rnp_failure(rnp_recipient_get_alg(recipient, NULL));
    assert_rnp_success(rnp_recipient_get_alg(recipient, &alg));
    assert_string_equal(alg, "RSA");
    rnp_buffer_destroy(alg);
    char *keyid = NULL;
    assert_rnp_failure(rnp_recipient_get_keyid(NULL, &keyid));
    assert_rnp_failure(rnp_recipient_get_keyid(recipient, NULL));
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_string_equal(keyid, "8A05B89FAD5ADED1");
    rnp_buffer_destroy(keyid);
    recipient = NULL;
    assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
    assert_non_null(recipient);
    alg = NULL;
    assert_rnp_success(rnp_recipient_get_alg(recipient, &alg));
    assert_string_equal(alg, "RSA");
    rnp_buffer_destroy(alg);
    keyid = NULL;
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_string_equal(keyid, "8A05B89FAD5ADED1");
    rnp_buffer_destroy(keyid);
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 0);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message with AEAD-OCB: single password */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-ocb"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    if (!aead_ocb_enabled() || aead_ocb_aes_only()) {
        assert_rnp_failure(rnp_op_verify_execute(verify));
    } else {
        assert_rnp_success(rnp_op_verify_execute(verify));
    }
    count = 255;
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 1);
    assert_rnp_failure(rnp_op_verify_get_symenc_at(verify, 1, &symenc));
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 0, &symenc));
    assert_non_null(symenc);
    char *cipher = NULL;
    assert_rnp_failure(rnp_symenc_get_cipher(symenc, NULL));
    assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
    assert_string_equal(cipher, "CAMELLIA192");
    rnp_buffer_destroy(cipher);
    char *aead = NULL;
    assert_rnp_failure(rnp_symenc_get_aead_alg(symenc, NULL));
    assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aead));
    assert_string_equal(aead, "OCB");
    rnp_buffer_destroy(aead);
    char *hash = NULL;
    assert_rnp_failure(rnp_symenc_get_hash_alg(symenc, NULL));
    assert_rnp_success(rnp_symenc_get_hash_alg(symenc, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    char *s2k = NULL;
    assert_rnp_failure(rnp_symenc_get_s2k_type(symenc, NULL));
    assert_rnp_success(rnp_symenc_get_s2k_type(symenc, &s2k));
    assert_string_equal(s2k, "Iterated and salted");
    rnp_buffer_destroy(s2k);
    uint32_t iterations = 0;
    assert_rnp_failure(rnp_symenc_get_s2k_iterations(symenc, NULL));
    assert_rnp_success(rnp_symenc_get_s2k_iterations(symenc, &iterations));
    assert_int_equal(iterations, 30408704);
    assert_rnp_success(rnp_op_verify_get_used_symenc(verify, &symenc));
    if (!aead_ocb_enabled() || aead_ocb_aes_only()) {
        assert_null(symenc);
    } else {
        assert_non_null(symenc);
        cipher = NULL;
        assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
        assert_string_equal(cipher, "CAMELLIA192");
        rnp_buffer_destroy(cipher);
        aead = NULL;
        assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aead));
        assert_string_equal(aead, "OCB");
        rnp_buffer_destroy(aead);
        hash = NULL;
        assert_rnp_success(rnp_symenc_get_hash_alg(symenc, &hash));
        assert_string_equal(hash, "SHA1");
        rnp_buffer_destroy(hash);
        s2k = NULL;
        assert_rnp_success(rnp_symenc_get_s2k_type(symenc, &s2k));
        assert_string_equal(s2k, "Iterated and salted");
        rnp_buffer_destroy(s2k);
        iterations = 0;
        assert_rnp_success(rnp_symenc_get_s2k_iterations(symenc, &iterations));
        assert_int_equal(iterations, 30408704);
    }
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message with AEAD-OCB-AES: single password */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-ocb-aes"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    if (!aead_ocb_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(verify));
    } else {
        assert_rnp_success(rnp_op_verify_execute(verify));
    }
    count = 255;
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 0, &symenc));
    assert_non_null(symenc);
    cipher = NULL;
    assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
    assert_string_equal(cipher, "AES192");
    rnp_buffer_destroy(cipher);
    aead = NULL;
    assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aead));
    assert_string_equal(aead, "OCB");
    rnp_buffer_destroy(aead);
    assert_rnp_success(rnp_op_verify_get_used_symenc(verify, &symenc));
    if (!aead_ocb_enabled()) {
        assert_null(symenc);
    } else {
        assert_non_null(symenc);
        cipher = NULL;
        assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
        assert_string_equal(cipher, "AES192");
        rnp_buffer_destroy(cipher);
        aead = NULL;
        assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aead));
        assert_string_equal(aead, "OCB");
        rnp_buffer_destroy(aead);
    }
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* modified message with AEAD-EAX: one recipient and one password, decrypt with recipient
     */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-eax-malf"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    count = 255;
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_op_verify_get_recipient_at(verify, 0, &recipient));
    assert_non_null(recipient);
    alg = NULL;
    assert_rnp_success(rnp_recipient_get_alg(recipient, &alg));
    assert_string_equal(alg, "RSA");
    rnp_buffer_destroy(alg);
    keyid = NULL;
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_string_equal(keyid, "1ED63EE56FADC34D");
    rnp_buffer_destroy(keyid);
    recipient = NULL;
    assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
    if (!aead_eax_enabled()) {
        assert_null(recipient);
    } else {
        assert_non_null(recipient);
        assert_rnp_success(rnp_recipient_get_alg(recipient, &alg));
        assert_string_equal(alg, "RSA");
        rnp_buffer_destroy(alg);
        assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
        assert_string_equal(keyid, "1ED63EE56FADC34D");
        rnp_buffer_destroy(keyid);
        assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
        assert_non_null(recipient);
        alg = NULL;
        assert_rnp_success(rnp_recipient_get_alg(recipient, &alg));
        assert_string_equal(alg, "RSA");
        rnp_buffer_destroy(alg);
        keyid = NULL;
        assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
        assert_string_equal(keyid, "1ED63EE56FADC34D");
        rnp_buffer_destroy(keyid);
        recipient = NULL;
        assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
        assert_non_null(recipient);
        assert_rnp_success(rnp_recipient_get_alg(recipient, &alg));
        assert_string_equal(alg, "RSA");
        rnp_buffer_destroy(alg);
        assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
        assert_string_equal(keyid, "1ED63EE56FADC34D");
        rnp_buffer_destroy(keyid);
    }

    count = 255;
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 0, &symenc));
    assert_non_null(symenc);
    cipher = NULL;
    assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
    assert_string_equal(cipher, "AES256");
    rnp_buffer_destroy(cipher);
    aead = NULL;
    assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aead));
    assert_string_equal(aead, "EAX");
    rnp_buffer_destroy(aead);
    hash = NULL;
    assert_rnp_success(rnp_symenc_get_hash_alg(symenc, &hash));
    assert_string_equal(hash, "SHA256");
    rnp_buffer_destroy(hash);
    s2k = NULL;
    assert_rnp_success(rnp_symenc_get_s2k_type(symenc, &s2k));
    assert_string_equal(s2k, "Iterated and salted");
    rnp_buffer_destroy(s2k);
    iterations = 0;
    assert_rnp_success(rnp_symenc_get_s2k_iterations(symenc, &iterations));
    assert_int_equal(iterations, 3932160);
    assert_rnp_success(rnp_op_verify_get_used_symenc(verify, &symenc));
    assert_null(symenc);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message with AEAD-EAX: one recipient and one password, decrypt with password */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-aead-eax"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    if (!aead_eax_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(verify));
    } else {
        assert_rnp_success(rnp_op_verify_execute(verify));
    }
    count = 255;
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
    assert_null(recipient);
    count = 255;
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 0, &symenc));
    assert_non_null(symenc);
    assert_rnp_success(rnp_op_verify_get_used_symenc(verify, &symenc));
    if (!aead_eax_enabled()) {
        assert_null(symenc);
    } else {
        assert_non_null(symenc);
        cipher = NULL;
        assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
        assert_string_equal(cipher, "AES256");
        rnp_buffer_destroy(cipher);
        aead = NULL;
        assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aead));
        assert_string_equal(aead, "EAX");
        rnp_buffer_destroy(aead);
        hash = NULL;
        assert_rnp_success(rnp_symenc_get_hash_alg(symenc, &hash));
        assert_string_equal(hash, "SHA256");
        rnp_buffer_destroy(hash);
        s2k = NULL;
        assert_rnp_success(rnp_symenc_get_s2k_type(symenc, &s2k));
        assert_string_equal(s2k, "Iterated and salted");
        rnp_buffer_destroy(s2k);
        iterations = 0;
        assert_rnp_success(rnp_symenc_get_s2k_iterations(symenc, &iterations));
        assert_int_equal(iterations, 3932160);
    }
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* message encrypted to 3 recipients and 2 passwords: password1, password2 */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "wrongpassword"));
    assert_true(import_sec_keys(ffi, "data/keyrings/1/secring.gpg"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-3key-2p"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    count = 255;
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 3);
    assert_rnp_success(rnp_op_verify_get_recipient_at(verify, 0, &recipient));
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_string_equal(keyid, "1ED63EE56FADC34D");
    rnp_buffer_destroy(keyid);
    assert_rnp_success(rnp_op_verify_get_recipient_at(verify, 1, &recipient));
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_string_equal(keyid, "8A05B89FAD5ADED1");
    rnp_buffer_destroy(keyid);
    assert_rnp_success(rnp_op_verify_get_recipient_at(verify, 2, &recipient));
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_string_equal(keyid, "54505A936A4A970E");
    rnp_buffer_destroy(keyid);
    assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
    assert_null(recipient);
    count = 255;
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 2);
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 0, &symenc));
    assert_rnp_success(rnp_symenc_get_s2k_iterations(symenc, &iterations));
    assert_int_equal(iterations, 3932160);
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 1, &symenc));
    assert_rnp_success(rnp_symenc_get_s2k_iterations(symenc, &iterations));
    assert_int_equal(iterations, 3276800);
    assert_rnp_success(rnp_op_verify_get_used_symenc(verify, &symenc));
    assert_null(symenc);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password2"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-3key-2p"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_used_symenc(verify, &symenc));
    assert_rnp_success(rnp_symenc_get_s2k_iterations(symenc, &iterations));
    assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
    assert_null(recipient);
    assert_int_equal(iterations, 3276800);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, getpasscb_for_key, (void *) "8A05B89FAD5ADED1"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-3key-2p"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_used_symenc(verify, &symenc));
    assert_null(symenc);
    assert_rnp_success(rnp_op_verify_get_used_recipient(verify, &recipient));
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_string_equal(keyid, "8A05B89FAD5ADED1");
    rnp_buffer_destroy(keyid);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_secret_sig_import)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sec.asc"));
    rnp_key_handle_t key_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    bool locked = false;
    /* unlock secret key */
    assert_rnp_success(rnp_key_is_locked(key_handle, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_unlock(key_handle, "password"));
    assert_rnp_success(rnp_key_is_locked(key_handle, &locked));
    assert_false(locked);
    /* import revocation signature */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_key_validity/alice-rev.pgp"));
    assert_rnp_success(rnp_import_signatures(ffi, input, 0, NULL));
    assert_rnp_success(rnp_input_destroy(input));
    /* make sure that key is still unlocked */
    assert_rnp_success(rnp_key_is_locked(key_handle, &locked));
    assert_false(locked);
    /* import subkey */
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sub-sec.pgp"));
    /* make sure that primary key is still unlocked */
    assert_rnp_success(rnp_key_is_locked(key_handle, &locked));
    assert_false(locked);
    /* unlock subkey and make sure it is unlocked after revocation */
    rnp_key_handle_t sub_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &sub_handle));
    assert_rnp_success(rnp_key_unlock(sub_handle, "password"));
    assert_rnp_success(rnp_key_is_locked(sub_handle, &locked));
    assert_false(locked);
    assert_rnp_success(rnp_key_revoke(sub_handle, 0, "SHA256", "retired", "Custom reason"));
    assert_rnp_success(rnp_key_is_locked(sub_handle, &locked));
    assert_false(locked);
    assert_rnp_success(rnp_key_handle_destroy(sub_handle));
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

static bool
getpasscb_fail(rnp_ffi_t        ffi,
               void *           app_ctx,
               rnp_key_handle_t key,
               const char *     pgp_context,
               char *           buf,
               size_t           buf_len)
{
    return false;
}

static bool
getpasscb_context(rnp_ffi_t        ffi,
                  void *           app_ctx,
                  rnp_key_handle_t key,
                  const char *     pgp_context,
                  char *           buf,
                  size_t           buf_len)
{
    strncpy(buf, pgp_context, buf_len - 1);
    return true;
}

static bool
getpasscb_keyid(rnp_ffi_t        ffi,
                void *           app_ctx,
                rnp_key_handle_t key,
                const char *     pgp_context,
                char *           buf,
                size_t           buf_len)
{
    if (!key) {
        return false;
    }
    char *keyid = NULL;
    if (rnp_key_get_keyid(key, &keyid)) {
        return false;
    }
    strncpy(buf, keyid, buf_len - 1);
    rnp_buffer_destroy(keyid);
    return true;
}

TEST_F(rnp_tests, test_ffi_rnp_request_password)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* check wrong parameters cases */
    char *password = NULL;
    assert_rnp_failure(rnp_request_password(ffi, NULL, "sign", &password));
    assert_null(password);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_failure(rnp_request_password(NULL, NULL, "sign", &password));
    assert_rnp_failure(rnp_request_password(ffi, NULL, "sign", NULL));
    /* now it should succeed */
    assert_rnp_success(rnp_request_password(ffi, NULL, "sign", &password));
    assert_string_equal(password, "password");
    rnp_buffer_destroy(password);
    /* let's try failing password provider */
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, getpasscb_fail, NULL));
    assert_rnp_failure(rnp_request_password(ffi, NULL, "sign", &password));
    /* let's try to return request context */
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, getpasscb_context, NULL));
    assert_rnp_success(rnp_request_password(ffi, NULL, "custom context", &password));
    assert_string_equal(password, "custom context");
    rnp_buffer_destroy(password);
    /* let's check whether key is correctly passed to handler */
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sec.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, getpasscb_keyid, NULL));
    assert_rnp_success(rnp_request_password(ffi, key, NULL, &password));
    assert_string_equal(password, "0451409669FFDE3C");
    rnp_buffer_destroy(password);
    assert_rnp_success(rnp_key_handle_destroy(key));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_mdc_8k_boundary)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;

    test_ffi_init(&ffi);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    /* correctly process two messages */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message_mdc_8k_1.pgp"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    /* check signature */
    size_t sig_count = 0;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sig_count));
    assert_int_equal(sig_count, 1);
    rnp_op_verify_signature_t sig = NULL;
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    /* cleanup */
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));

    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message_mdc_8k_2.pgp"));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    /* check signature */
    sig_count = 0;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sig_count));
    assert_int_equal(sig_count, 1);
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    /* cleanup */
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));

    /* let it gracefully fail on message 1 with the last byte cut */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message_mdc_8k_cut1.pgp"));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    /* cleanup */
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));

    /* let it gracefully fail on message 1 with the last 22 bytes (MDC size) cut */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message_mdc_8k_cut22.pgp"));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    /* cleanup */
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));

    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_decrypt_wrong_mpi_bits)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    // init ffi
    test_ffi_init(&ffi);

    /* 1024 bitcount instead of 1023 */
    rnp_op_verify_t op = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-malf-1"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    if (!aead_eax_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(op));
    } else {
        assert_rnp_success(rnp_op_verify_execute(op));
    }
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* 1025 bitcount instead of 1023 */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-malf-2"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    if (!aead_eax_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(op));
    } else {
        assert_rnp_success(rnp_op_verify_execute(op));
    }
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* 1031 bitcount instead of 1023 */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-malf-3"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    if (!aead_eax_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(op));
    } else {
        assert_rnp_success(rnp_op_verify_execute(op));
    }
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* 1040 bitcount instead of 1023 */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-malf-4"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    if (!aead_eax_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(op));
    } else {
        assert_rnp_success(rnp_op_verify_execute(op));
    }
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* 1017 bitcount instead of 1023 */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-malf-5"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    if (!aead_eax_enabled()) {
        assert_rnp_failure(rnp_op_verify_execute(op));
    } else {
        assert_rnp_success(rnp_op_verify_execute(op));
    }
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_decrypt_edge_cases)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    test_ffi_init(&ffi);

    /* unknown algorithm in public-key encrypted session key */
    rnp_op_verify_t op = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-wrong-alg"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(op));
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* endless recursive compression packets, 'quine'.
     * Generated using the code by Taylor R. Campbell */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.zlib-quine"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(op));
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.zlib-quine"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_dump_packets_to_output(input, output, 0));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.zlib-quine"));
    char *json = NULL;
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    assert_non_null(json);
    rnp_buffer_destroy(json);
    rnp_input_destroy(input);

    /* 128 levels of compression - fail decryption*/
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr.128-rounds"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(op));
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* but dumping will succeed */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr.128-rounds"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dump_packets_to_output(input, output, 0));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr.128-rounds"));
    json = NULL;
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    assert_non_null(json);
    rnp_buffer_destroy(json);
    rnp_input_destroy(input);

    /* 32 levels of compression + encryption */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr-encr.32-rounds"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(op));
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr-encr.32-rounds"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dump_packets_to_output(input, output, 0));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr-encr.32-rounds"));
    json = NULL;
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    assert_non_null(json);
    rnp_buffer_destroy(json);
    rnp_input_destroy(input);

    /* 31 levels of compression + encryption */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr-encr.31-rounds"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_op_verify_create(&op, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(op));
    rnp_op_verify_destroy(op);
    rnp_input_destroy(input);
    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 7);
    assert_int_equal(memcmp(buf, "message", 7), 0);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr-encr.31-rounds"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_dump_packets_to_output(input, output, 0));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.compr-encr.31-rounds"));
    json = NULL;
    assert_rnp_success(rnp_dump_packets_to_json(input, 0, &json));
    assert_non_null(json);
    rnp_buffer_destroy(json);
    rnp_input_destroy(input);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_remove)
{
    rnp_ffi_t ffi = NULL;
    test_ffi_init(&ffi);

    rnp_key_handle_t key0 = NULL;
    rnp_key_handle_t key0_sub0 = NULL;
    rnp_key_handle_t key0_sub1 = NULL;
    rnp_key_handle_t key0_sub2 = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key0));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1ed63ee56fadc34d", &key0_sub0));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1d7e8a5393c997a8", &key0_sub1));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8a05b89fad5aded1", &key0_sub2));

    /* edge cases */
    assert_rnp_failure(rnp_key_remove(NULL, RNP_KEY_REMOVE_PUBLIC));
    assert_rnp_failure(rnp_key_remove(key0, 0));
    /* make sure we correctly remove public and secret keys */
    bool pub = false;
    assert_rnp_success(rnp_key_have_public(key0_sub2, &pub));
    assert_true(pub);
    bool sec = false;
    assert_rnp_success(rnp_key_have_secret(key0_sub2, &sec));
    assert_true(sec);
    assert_rnp_success(rnp_key_remove(key0_sub2, RNP_KEY_REMOVE_PUBLIC));
    pub = true;
    assert_rnp_success(rnp_key_have_public(key0_sub2, &pub));
    assert_false(pub);
    sec = false;
    assert_rnp_success(rnp_key_have_secret(key0_sub2, &sec));
    assert_true(sec);
    assert_rnp_failure(rnp_key_remove(key0_sub2, RNP_KEY_REMOVE_PUBLIC));
    rnp_key_handle_destroy(key0_sub2);
    /* locate it back */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8a05b89fad5aded1", &key0_sub2));
    assert_non_null(key0_sub2);
    pub = true;
    assert_rnp_success(rnp_key_have_public(key0_sub2, &pub));
    assert_false(pub);
    sec = false;
    assert_rnp_success(rnp_key_have_secret(key0_sub2, &sec));
    assert_true(sec);

    pub = false;
    assert_rnp_success(rnp_key_have_public(key0_sub0, &pub));
    assert_true(pub);
    sec = false;
    assert_rnp_success(rnp_key_have_secret(key0_sub0, &sec));
    assert_true(sec);
    assert_rnp_success(rnp_key_remove(key0_sub0, RNP_KEY_REMOVE_SECRET));
    pub = false;
    assert_rnp_success(rnp_key_have_public(key0_sub0, &pub));
    assert_true(pub);
    sec = true;
    assert_rnp_success(rnp_key_have_secret(key0_sub0, &sec));
    assert_false(sec);
    assert_rnp_failure(rnp_key_remove(key0_sub0, RNP_KEY_REMOVE_SECRET));
    rnp_key_handle_destroy(key0_sub0);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1ed63ee56fadc34d", &key0_sub0));
    assert_non_null(key0_sub0);

    size_t count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 6);
    count = 0;
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 6);

    /* while there are 2 public and 1 secret subkey, this calculates only public */
    assert_rnp_success(rnp_key_get_subkey_count(key0, &count));
    assert_int_equal(count, 2);

    assert_rnp_success(rnp_key_remove(key0_sub0, RNP_KEY_REMOVE_PUBLIC));
    assert_rnp_success(rnp_key_get_subkey_count(key0, &count));
    assert_int_equal(count, 1);
    count = 0;
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 5);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 6);

    assert_rnp_success(rnp_key_remove(key0_sub2, RNP_KEY_REMOVE_SECRET));
    assert_rnp_success(rnp_key_get_subkey_count(key0, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 5);

    assert_rnp_success(rnp_key_remove(key0, RNP_KEY_REMOVE_PUBLIC | RNP_KEY_REMOVE_SECRET));
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 4);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 4);

    rnp_key_handle_destroy(key0_sub1);
    /* key0_sub1 should be left in keyring */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1d7e8a5393c997a8", &key0_sub1));
    pub = false;
    assert_rnp_success(rnp_key_have_public(key0_sub1, &pub));
    assert_true(pub);
    sec = false;
    assert_rnp_success(rnp_key_have_secret(key0_sub1, &sec));
    assert_true(sec);

    rnp_key_handle_destroy(key0);
    rnp_key_handle_destroy(key0_sub0);
    rnp_key_handle_destroy(key0_sub1);
    rnp_key_handle_destroy(key0_sub2);

    /* let's import keys back */
    assert_true(import_pub_keys(ffi, "data/keyrings/1/pubring.gpg"));
    assert_true(import_sec_keys(ffi, "data/keyrings/1/secring.gpg"));

    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 7);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 7);

    /* now try to remove the whole key */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key0));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "1ed63ee56fadc34d", &key0_sub0));

    assert_rnp_failure(
      rnp_key_remove(key0_sub0, RNP_KEY_REMOVE_SECRET | RNP_KEY_REMOVE_SUBKEYS));
    assert_rnp_success(rnp_key_remove(key0_sub0, RNP_KEY_REMOVE_SECRET));
    assert_rnp_success(rnp_key_remove(key0_sub0, RNP_KEY_REMOVE_PUBLIC));

    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 6);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 6);

    assert_rnp_success(rnp_key_remove(key0, RNP_KEY_REMOVE_SECRET | RNP_KEY_REMOVE_SUBKEYS));
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 6);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 3);

    assert_rnp_success(rnp_key_remove(key0, RNP_KEY_REMOVE_PUBLIC | RNP_KEY_REMOVE_SUBKEYS));
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 3);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 3);

    rnp_key_handle_destroy(key0);
    rnp_key_handle_destroy(key0_sub0);

    /* delete the second key all at once */
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "2fcadf05ffa501bb", &key0));
    assert_rnp_success(rnp_key_remove(
      key0, RNP_KEY_REMOVE_PUBLIC | RNP_KEY_REMOVE_SECRET | RNP_KEY_REMOVE_SUBKEYS));
    assert_rnp_success(rnp_get_public_key_count(ffi, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_get_secret_key_count(ffi, &count));
    assert_int_equal(count, 0);
    rnp_key_handle_destroy(key0);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_literal_packet)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    // init ffi
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* try rnp_decrypt() */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.literal"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    uint8_t *buf = NULL;
    size_t   len = 0;
    rnp_output_memory_get_buf(output, &buf, &len, false);
    std::string out;
    out.assign((char *) buf, len);
    assert_true(out == file_to_str("data/test_messages/message.txt"));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* try rnp_op_verify() */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.literal"));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    rnp_output_memory_get_buf(output, &buf, &len, false);
    out.assign((char *) buf, len);
    assert_true(out == file_to_str("data/test_messages/message.txt"));
    char *mode = NULL;
    char *cipher = NULL;
    bool  valid = true;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "none");
    assert_string_equal(cipher, "none");
    assert_false(valid);
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    size_t count = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &count));
    assert_int_equal(count, 0);
    count = 255;
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 0);
    count = 255;
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 0);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}

/* This test checks that any exceptions thrown by the internal library
 * will not propagate beyond the FFI boundary.
 * In this case we (ab)use a callback to mimic this scenario.
 */
TEST_F(rnp_tests, test_ffi_exception)
{
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    // bad_alloc -> RNP_ERROR_OUT_OF_MEMORY
    {
        auto reader = [](void *app_ctx, void *buf, size_t len, size_t *read) {
            throw std::bad_alloc();
            return true;
        };
        assert_rnp_failure(rnp_input_from_callback(NULL, reader, NULL, NULL));
        assert_rnp_failure(rnp_input_from_callback(&input, NULL, NULL, NULL));
        assert_rnp_success(rnp_input_from_callback(&input, reader, NULL, NULL));
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_int_equal(RNP_ERROR_OUT_OF_MEMORY, rnp_output_pipe(input, output));
        rnp_input_destroy(input);
        rnp_output_destroy(output);
    }

    // runtime_error -> RNP_ERROR_GENERIC
    {
        auto reader = [](void *app_ctx, void *buf, size_t len, size_t *read) {
            throw std::runtime_error("");
            return true;
        };
        assert_rnp_success(rnp_input_from_callback(&input, reader, NULL, NULL));
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_int_equal(RNP_ERROR_GENERIC, rnp_output_pipe(input, output));
        rnp_input_destroy(input);
        rnp_output_destroy(output);
    }

    // everything else -> RNP_ERROR_GENERIC
    {
        auto reader = [](void *app_ctx, void *buf, size_t len, size_t *read) {
            throw 5;
            return true;
        };
        assert_rnp_success(rnp_input_from_callback(&input, reader, NULL, NULL));
        assert_rnp_success(rnp_output_to_memory(&output, 0));
        assert_int_equal(RNP_ERROR_GENERIC, rnp_output_pipe(input, output));
        rnp_input_destroy(input);
        rnp_output_destroy(output);
    }
}

TEST_F(rnp_tests, test_ffi_key_protection_change)
{
    rnp_ffi_t ffi = NULL;
    test_ffi_init(&ffi);

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8a05b89fad5aded1", &sub));

    assert_rnp_success(rnp_key_unprotect(key, "password"));
    assert_rnp_success(rnp_key_unprotect(sub, "password"));

    bool protect = true;
    assert_rnp_success(rnp_key_is_protected(key, &protect));
    assert_false(protect);
    protect = true;
    assert_rnp_success(rnp_key_is_protected(sub, &protect));
    assert_false(protect);

    assert_rnp_success(rnp_key_protect(key, "password2", "Camellia128", NULL, "SHA1", 0));
    assert_rnp_success(rnp_key_protect(sub, "password2", "Camellia256", NULL, "SHA512", 0));
    protect = false;
    assert_rnp_success(rnp_key_is_protected(key, &protect));
    assert_true(protect);
    protect = false;
    assert_rnp_success(rnp_key_is_protected(sub, &protect));
    assert_true(protect);

    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);

    reload_keyrings(&ffi);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8a05b89fad5aded1", &sub));

    protect = false;
    assert_rnp_success(rnp_key_is_protected(key, &protect));
    assert_true(protect);
    protect = false;
    assert_rnp_success(rnp_key_is_protected(sub, &protect));
    assert_true(protect);

    char *cipher = NULL;
    assert_rnp_success(rnp_key_get_protection_cipher(key, &cipher));
    assert_string_equal(cipher, "CAMELLIA128");
    rnp_buffer_destroy(cipher);
    char *hash = NULL;
    assert_rnp_success(rnp_key_get_protection_hash(key, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    cipher = NULL;
    assert_rnp_success(rnp_key_get_protection_cipher(sub, &cipher));
    assert_string_equal(cipher, "CAMELLIA256");
    rnp_buffer_destroy(cipher);
    hash = NULL;
    assert_rnp_success(rnp_key_get_protection_hash(sub, &hash));
    assert_string_equal(hash, "SHA512");
    rnp_buffer_destroy(hash);

    assert_rnp_failure(rnp_key_unlock(key, "password"));
    assert_rnp_failure(rnp_key_unlock(sub, "password"));

    assert_rnp_success(rnp_key_unlock(key, "password2"));
    assert_rnp_success(rnp_key_unlock(sub, "password2"));

    protect = false;
    assert_rnp_success(rnp_key_is_protected(key, &protect));
    assert_true(protect);
    protect = false;
    assert_rnp_success(rnp_key_is_protected(sub, &protect));
    assert_true(protect);

    assert_rnp_success(rnp_key_protect(key, "password3", "AES256", NULL, "SHA512", 10000000));
    assert_rnp_success(rnp_key_protect(sub, "password3", "AES128", NULL, "SHA1", 10000000));
    protect = false;
    assert_rnp_success(rnp_key_is_protected(key, &protect));
    assert_true(protect);
    protect = false;
    assert_rnp_success(rnp_key_is_protected(sub, &protect));
    assert_true(protect);

    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);

    reload_keyrings(&ffi);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8a05b89fad5aded1", &sub));

    protect = false;
    assert_rnp_success(rnp_key_is_protected(key, &protect));
    assert_true(protect);
    protect = false;
    assert_rnp_success(rnp_key_is_protected(sub, &protect));
    assert_true(protect);

    cipher = NULL;
    assert_rnp_success(rnp_key_get_protection_cipher(key, &cipher));
    assert_string_equal(cipher, "AES256");
    rnp_buffer_destroy(cipher);
    hash = NULL;
    assert_rnp_success(rnp_key_get_protection_hash(key, &hash));
    assert_string_equal(hash, "SHA512");
    rnp_buffer_destroy(hash);
    size_t iterations = 0;
    assert_rnp_success(rnp_key_get_protection_iterations(key, &iterations));
    assert_true(iterations >= 10000000);
    cipher = NULL;
    assert_rnp_success(rnp_key_get_protection_cipher(sub, &cipher));
    assert_string_equal(cipher, "AES128");
    rnp_buffer_destroy(cipher);
    hash = NULL;
    assert_rnp_success(rnp_key_get_protection_hash(sub, &hash));
    assert_string_equal(hash, "SHA1");
    rnp_buffer_destroy(hash);
    iterations = 0;
    assert_rnp_success(rnp_key_get_protection_iterations(sub, &iterations));
    assert_true(iterations >= 10000000);

    assert_rnp_failure(rnp_key_unlock(key, "password"));
    assert_rnp_failure(rnp_key_unlock(sub, "password"));

    assert_rnp_success(rnp_key_unlock(key, "password3"));
    assert_rnp_success(rnp_key_unlock(sub, "password3"));

    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_set_log_fd)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_failure(rnp_ffi_set_log_fd(NULL, 0));
    assert_rnp_failure(rnp_ffi_set_log_fd(ffi, 100));
    int file_fd = rnp_open("tests.txt", O_RDWR | O_CREAT | O_TRUNC, 0777);
    assert_true(file_fd > 0);
    assert_rnp_success(rnp_ffi_set_log_fd(ffi, file_fd));
    rnp_input_t input = NULL;
    const char *msg = "hello";
    assert_rnp_success(
      rnp_input_from_memory(&input, (const uint8_t *) msg, strlen(msg), true));
    char *saved_env = NULL;
    if (getenv(RNP_LOG_CONSOLE)) {
        saved_env = strdup(getenv(RNP_LOG_CONSOLE));
    }
    setenv(RNP_LOG_CONSOLE, "1", 1);
    assert_rnp_failure(rnp_load_keys(ffi, "GPG", input, 119));
    assert_rnp_success(rnp_input_destroy(input));
    rnp_ffi_destroy(ffi);
    if (saved_env) {
        assert_int_equal(0, setenv(RNP_LOG_CONSOLE, saved_env, 1));
        free(saved_env);
    } else {
        unsetenv(RNP_LOG_CONSOLE);
    }
#ifndef _WIN32
    assert_int_equal(fcntl(file_fd, F_GETFD), -1);
    assert_int_equal(errno, EBADF);
#endif
    char buffer[128] = {0};
    file_fd = rnp_open("tests.txt", O_RDONLY, 0);
    int64_t rres = read(file_fd, buffer, sizeof(buffer));
    assert_true(rres > 0);
    assert_non_null(strstr(buffer, "unexpected flags remaining: 0x"));
    close(file_fd);
}

TEST_F(rnp_tests, test_ffi_security_profile)
{
    rnp_ffi_t ffi = NULL;
    rnp_ffi_create(&ffi, "GPG", "GPG");
    /* check predefined rules */
    uint32_t flags = 0;
    uint64_t from = 0;
    uint32_t level = 0;
    assert_rnp_failure(
      rnp_get_security_rule(NULL, RNP_FEATURE_HASH_ALG, "SHA1", 0, &flags, &from, &level));
    assert_rnp_failure(rnp_get_security_rule(ffi, NULL, "SHA1", 0, &flags, &from, &level));
    assert_rnp_success(
      rnp_get_security_rule(ffi, RNP_FEATURE_SYMM_ALG, "AES256", 0, &flags, &from, &level));
    assert_rnp_failure(
      rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "Unknown", 0, &flags, &from, &level));
    assert_rnp_failure(
      rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "SHA1", 0, NULL, NULL, NULL));
    /* default rule */
    from = 256;
    assert_rnp_success(
      rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "SHA256", 0, &flags, &from, &level));
    assert_int_equal(level, RNP_SECURITY_DEFAULT);
    assert_int_equal(from, 0);
    /* MD5 */
    from = 256;
    assert_rnp_success(
      rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "MD5", 0, &flags, &from, &level));
    assert_int_equal(from, 0);
    assert_int_equal(level, RNP_SECURITY_DEFAULT);
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "MD5", time(NULL), &flags, &from, &level));
    assert_int_equal(from, MD5_FROM);
    assert_int_equal(level, RNP_SECURITY_INSECURE);
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "MD5", MD5_FROM - 1, &flags, &from, &level));
    assert_int_equal(from, 0);
    assert_int_equal(flags, 0);
    assert_int_equal(level, RNP_SECURITY_DEFAULT);
    /* Override it */
    assert_rnp_failure(rnp_add_security_rule(
      NULL, RNP_FEATURE_HASH_ALG, "MD5", RNP_SECURITY_OVERRIDE, 0, RNP_SECURITY_DEFAULT));
    assert_rnp_failure(rnp_add_security_rule(
      ffi, RNP_FEATURE_SYMM_ALG, "MD5", RNP_SECURITY_OVERRIDE, 0, RNP_SECURITY_DEFAULT));
    assert_rnp_failure(
      rnp_add_security_rule(ffi, NULL, "MD5", RNP_SECURITY_OVERRIDE, 0, RNP_SECURITY_DEFAULT));
    assert_rnp_failure(rnp_add_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, NULL, RNP_SECURITY_OVERRIDE, 0, RNP_SECURITY_DEFAULT));
    assert_rnp_failure(rnp_add_security_rule(ffi,
                                             RNP_FEATURE_HASH_ALG,
                                             "MD5",
                                             RNP_SECURITY_OVERRIDE | 0x17,
                                             0,
                                             RNP_SECURITY_DEFAULT));
    assert_rnp_failure(
      rnp_add_security_rule(ffi, RNP_FEATURE_HASH_ALG, "MD5", RNP_SECURITY_OVERRIDE, 0, 25));
    assert_rnp_success(rnp_add_security_rule(ffi,
                                             RNP_FEATURE_HASH_ALG,
                                             "MD5",
                                             RNP_SECURITY_OVERRIDE,
                                             MD5_FROM - 1,
                                             RNP_SECURITY_DEFAULT));
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "MD5", time(NULL), &flags, &from, &level));
    assert_int_equal(from, MD5_FROM - 1);
    assert_int_equal(flags, RNP_SECURITY_OVERRIDE);
    assert_int_equal(level, RNP_SECURITY_DEFAULT);
    /* Remove and check back */
    size_t removed = 0;
    assert_rnp_failure(rnp_remove_security_rule(NULL,
                                                RNP_FEATURE_HASH_ALG,
                                                "MD5",
                                                RNP_SECURITY_DEFAULT,
                                                RNP_SECURITY_OVERRIDE,
                                                MD5_FROM - 1,
                                                &removed));
    assert_rnp_failure(rnp_remove_security_rule(ffi,
                                                RNP_FEATURE_SYMM_ALG,
                                                "MD5",
                                                RNP_SECURITY_DEFAULT,
                                                RNP_SECURITY_OVERRIDE,
                                                MD5_FROM - 1,
                                                &removed));
    assert_rnp_success(rnp_remove_security_rule(ffi,
                                                RNP_FEATURE_HASH_ALG,
                                                "SHA256",
                                                RNP_SECURITY_DEFAULT,
                                                RNP_SECURITY_OVERRIDE,
                                                MD5_FROM - 1,
                                                &removed));
    assert_int_equal(removed, 0);
    removed = 1;
    assert_rnp_success(rnp_remove_security_rule(ffi,
                                                RNP_FEATURE_HASH_ALG,
                                                "MD5",
                                                RNP_SECURITY_INSECURE,
                                                RNP_SECURITY_OVERRIDE,
                                                MD5_FROM - 1,
                                                &removed));
    assert_int_equal(removed, 0);
    removed = 1;
    assert_rnp_success(rnp_remove_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "MD5", RNP_SECURITY_DEFAULT, 0, MD5_FROM - 1, &removed));
    assert_int_equal(removed, 0);
    removed = 1;
    assert_rnp_success(rnp_remove_security_rule(ffi,
                                                RNP_FEATURE_HASH_ALG,
                                                "MD5",
                                                RNP_SECURITY_DEFAULT,
                                                RNP_SECURITY_OVERRIDE,
                                                MD5_FROM,
                                                &removed));
    assert_int_equal(removed, 0);
    assert_rnp_success(rnp_remove_security_rule(ffi,
                                                RNP_FEATURE_HASH_ALG,
                                                "MD5",
                                                RNP_SECURITY_DEFAULT,
                                                RNP_SECURITY_OVERRIDE,
                                                MD5_FROM - 1,
                                                &removed));
    assert_int_equal(removed, 1);
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "MD5", time(NULL), &flags, &from, &level));
    assert_int_equal(from, MD5_FROM);
    assert_int_equal(level, RNP_SECURITY_INSECURE);
    assert_int_equal(flags, 0);
    /* Add for data sigs only */
    assert_rnp_success(rnp_add_security_rule(ffi,
                                             RNP_FEATURE_HASH_ALG,
                                             "MD5",
                                             RNP_SECURITY_VERIFY_DATA,
                                             MD5_FROM + 1,
                                             RNP_SECURITY_DEFAULT));
    flags = RNP_SECURITY_VERIFY_DATA;
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "MD5", time(NULL), &flags, &from, &level));
    assert_int_equal(from, MD5_FROM + 1);
    assert_int_equal(flags, RNP_SECURITY_VERIFY_DATA);
    assert_int_equal(level, RNP_SECURITY_DEFAULT);
    /* Add for key sigs only */
    assert_rnp_success(rnp_add_security_rule(ffi,
                                             RNP_FEATURE_HASH_ALG,
                                             "MD5",
                                             RNP_SECURITY_VERIFY_KEY,
                                             MD5_FROM + 2,
                                             RNP_SECURITY_DEFAULT));
    flags = RNP_SECURITY_VERIFY_KEY;
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "MD5", time(NULL), &flags, &from, &level));
    assert_int_equal(from, MD5_FROM + 2);
    assert_int_equal(flags, RNP_SECURITY_VERIFY_KEY);
    assert_int_equal(level, RNP_SECURITY_DEFAULT);
    /* Remove added two rules */
    assert_rnp_success(rnp_remove_security_rule(ffi,
                                                RNP_FEATURE_HASH_ALG,
                                                "MD5",
                                                RNP_SECURITY_DEFAULT,
                                                RNP_SECURITY_VERIFY_DATA,
                                                MD5_FROM + 1,
                                                &removed));
    assert_int_equal(removed, 1);
    assert_rnp_success(rnp_remove_security_rule(ffi,
                                                RNP_FEATURE_HASH_ALG,
                                                "MD5",
                                                RNP_SECURITY_DEFAULT,
                                                RNP_SECURITY_VERIFY_KEY,
                                                MD5_FROM + 2,
                                                &removed));
    assert_int_equal(removed, 1);
    /* Remove all */
    removed = 0;
    assert_rnp_failure(rnp_remove_security_rule(ffi, NULL, NULL, 0, 0x17, 0, &removed));
    assert_rnp_success(rnp_remove_security_rule(ffi, NULL, NULL, 0, 0, 0, &removed));
    assert_int_equal(removed, 3 /*HASH*/ + 4 /*SYMM*/);
    rnp_ffi_destroy(ffi);
    rnp_ffi_create(&ffi, "GPG", "GPG");
    /* Remove all rules for hash */
    removed = 0;
    assert_rnp_success(
      rnp_remove_security_rule(ffi, RNP_FEATURE_SYMM_ALG, NULL, 0, 0, 0, &removed));
    assert_int_equal(removed, 4);
    removed = 0;
    assert_rnp_success(
      rnp_remove_security_rule(ffi, RNP_FEATURE_HASH_ALG, NULL, 0, 0, 0, &removed));
    assert_int_equal(removed, 3);
    rnp_ffi_destroy(ffi);
    rnp_ffi_create(&ffi, "GPG", "GPG");
    /* Remove all rules for specific hash */
    assert_rnp_success(rnp_remove_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "MD5", 0, RNP_SECURITY_REMOVE_ALL, 0, &removed));
    assert_int_equal(removed, 1);
    assert_rnp_success(rnp_remove_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "SHA1", 0, RNP_SECURITY_REMOVE_ALL, 0, &removed));
    assert_int_equal(removed, 2);
    rnp_ffi_destroy(ffi);
    rnp_ffi_create(&ffi, "GPG", "GPG");
    /* SHA1 - ancient times */
    from = 256;
    flags = 0;
    assert_rnp_success(
      rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "SHA1", 0, &flags, &from, &level));
    assert_int_equal(from, 0);
    assert_int_equal(level, RNP_SECURITY_DEFAULT);
    assert_int_equal(flags, 0);
    /* SHA1 - now, data verify disabled, key sig verify is enabled */
    flags = 0;
    auto now = time(NULL);
    bool sha1_cutoff = now > SHA1_KEY_FROM;
    /* This would pick default rule closer to the date independent on usage */
    assert_rnp_success(
      rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "SHA1", now, &flags, &from, &level));
    auto expect_from = sha1_cutoff ? SHA1_KEY_FROM : SHA1_DATA_FROM;
    auto expect_usage = sha1_cutoff ? RNP_SECURITY_VERIFY_KEY : RNP_SECURITY_VERIFY_DATA;
    assert_int_equal(from, expect_from);
    assert_int_equal(level, RNP_SECURITY_INSECURE);
    assert_int_equal(flags, expect_usage);
    flags = 0;
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "SHA1", SHA1_DATA_FROM - 1, &flags, &from, &level));
    assert_int_equal(from, 0);
    assert_int_equal(level, RNP_SECURITY_DEFAULT);
    flags = RNP_SECURITY_VERIFY_DATA;
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "SHA1", time(NULL), &flags, &from, &level));
    assert_int_equal(from, SHA1_DATA_FROM);
    assert_int_equal(level, RNP_SECURITY_INSECURE);
    assert_int_equal(flags, RNP_SECURITY_VERIFY_DATA);
    flags = RNP_SECURITY_VERIFY_KEY;
    assert_rnp_success(
      rnp_get_security_rule(ffi, RNP_FEATURE_HASH_ALG, "SHA1", now, &flags, &from, &level));
    expect_from = sha1_cutoff ? SHA1_KEY_FROM : 0;
    auto expect_level = sha1_cutoff ? RNP_SECURITY_INSECURE : RNP_SECURITY_DEFAULT;
    expect_usage = sha1_cutoff ? RNP_SECURITY_VERIFY_KEY : 0;
    assert_int_equal(from, expect_from);
    assert_int_equal(level, expect_level);
    assert_int_equal(flags, expect_usage);
    flags = RNP_SECURITY_VERIFY_KEY;
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "SHA1", SHA1_KEY_FROM + 5, &flags, &from, &level));
    assert_int_equal(from, SHA1_KEY_FROM);
    assert_int_equal(level, RNP_SECURITY_INSECURE);
    assert_int_equal(flags, RNP_SECURITY_VERIFY_KEY);
    flags = 0;
    assert_rnp_success(rnp_get_security_rule(
      ffi, RNP_FEATURE_HASH_ALG, "SHA1", SHA1_KEY_FROM + 5, &flags, &from, &level));
    assert_int_equal(from, SHA1_KEY_FROM);
    assert_int_equal(level, RNP_SECURITY_INSECURE);
    assert_int_equal(flags, RNP_SECURITY_VERIFY_KEY);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_result_to_string)
{
    const char *          result_string = NULL;
    rnp_result_t          code;
    std::set<std::string> stringset;

    result_string = rnp_result_to_string(RNP_SUCCESS);
    assert_string_equal(result_string, "Success");

    result_string = rnp_result_to_string(1);
    assert_string_equal(result_string, "Unsupported error code");

    /* Cover all defined error code ranges,
     * check that each defined
     * code has corresponding unique string */

    std::vector<std::pair<rnp_result_t, rnp_result_t>> error_codes = {
      {RNP_ERROR_GENERIC, RNP_ERROR_NULL_POINTER},
      {RNP_ERROR_ACCESS, RNP_ERROR_WRITE},
      {RNP_ERROR_BAD_STATE, RNP_ERROR_SIGNATURE_UNKNOWN},
      {RNP_ERROR_NOT_ENOUGH_DATA, RNP_ERROR_EOF}};

    for (auto &range : error_codes) {
        for (code = range.first; code <= range.second; code++) {
            result_string = rnp_result_to_string(code);

            assert_int_not_equal(strcmp("Unsupported error code", result_string), 0);

            auto search = stringset.find(result_string);

            /* Make sure returned error string is not already returned for other codes */
            assert_true(search == stringset.end());

            stringset.insert(result_string);
        }
    }
}

TEST_F(rnp_tests, test_ffi_wrong_hex_length)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    rnp_key_handle_t key = NULL;
    assert_rnp_failure(rnp_locate_key(ffi, "keyid", "BC6709B15C23A4A", &key));
    assert_rnp_failure(rnp_locate_key(ffi, "keyid", "C6709B15C23A4A", &key));
    rnp_ffi_destroy(ffi);
}
