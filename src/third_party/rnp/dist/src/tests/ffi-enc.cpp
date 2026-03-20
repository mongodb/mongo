/*
 * Copyright (c) 2020-2023 [Ribose Inc](https://www.ribose.com).
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

#include <rnp/rnp.h>
#include "rnp_tests.h"
#include "support.h"
#include "librepgp/stream-common.h"
#include "librepgp/stream-packet.h"
#include "librepgp/stream-sig.h"
#include <json.h>
#include <vector>
#include <string>
#include "file-utils.h"
#include <librepgp/stream-ctx.h>
#include "key.hpp"
#include "ffi-priv-types.h"

static bool
getpasscb_once(rnp_ffi_t        ffi,
               void *           app_ctx,
               rnp_key_handle_t key,
               const char *     pgp_context,
               char *           buf,
               size_t           buf_len)
{
    const char **pass = (const char **) app_ctx;
    if (!*pass) {
        return false;
    }
    size_t pass_len = strlen(*pass);
    if (pass_len >= buf_len) {
        return false;
    }
    memcpy(buf, *pass, pass_len);
    *pass = NULL;
    return true;
}

static bool
getpasscb_inc(rnp_ffi_t        ffi,
              void *           app_ctx,
              rnp_key_handle_t key,
              const char *     pgp_context,
              char *           buf,
              size_t           buf_len)
{
    int *       num = (int *) app_ctx;
    std::string pass = "pass" + std::to_string(*num);
    (*num)++;
    strncpy(buf, pass.c_str(), buf_len - 1);
    return true;
}

#define TBL_MAX_USERIDS 4
typedef struct key_tbl_t {
    const uint8_t *key_data;
    size_t         key_data_size;
    bool           secret;
    const char *   keyid;
    const char *   grip;
    const char *   userids[TBL_MAX_USERIDS + 1];
} key_tbl_t;

static void
tbl_getkeycb(rnp_ffi_t   ffi,
             void *      app_ctx,
             const char *identifier_type,
             const char *identifier,
             bool        secret)
{
    key_tbl_t *found = NULL;
    for (key_tbl_t *tbl = (key_tbl_t *) app_ctx; tbl && tbl->key_data && !found; tbl++) {
        if (tbl->secret != secret) {
            continue;
        }
        if (!strcmp(identifier_type, "keyid") && !strcmp(identifier, tbl->keyid)) {
            found = tbl;
            break;
        } else if (!strcmp(identifier_type, "grip") && !strcmp(identifier, tbl->grip)) {
            found = tbl;
            break;
        } else if (!strcmp(identifier_type, "userid")) {
            for (size_t i = 0; i < TBL_MAX_USERIDS; i++) {
                if (!strcmp(identifier, tbl->userids[i])) {
                    found = tbl;
                    break;
                }
            }
        }
    }
    if (found) {
        char *format = NULL;
        assert_rnp_success(
          rnp_detect_key_format(found->key_data, found->key_data_size, &format));
        assert_non_null(format);
        uint32_t    flags = secret ? RNP_LOAD_SAVE_SECRET_KEYS : RNP_LOAD_SAVE_PUBLIC_KEYS;
        rnp_input_t input = NULL;
        assert_rnp_success(
          rnp_input_from_memory(&input, found->key_data, found->key_data_size, true));
        assert_non_null(input);
        assert_rnp_success(rnp_load_keys(ffi, format, input, flags));
        free(format);
        assert_rnp_success(rnp_input_destroy(input));
        input = NULL;
    }
}

TEST_F(rnp_tests, test_ffi_encrypt_pass)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *     plaintext = "data1";

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    // load our keyrings
    assert_true(
      load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring.gpg"));

    // write out some data
    str_to_file("plaintext", plaintext);
    // create input+output w/ bad paths (should fail)
    input = NULL;
    assert_rnp_failure(rnp_input_from_stdin(NULL));
    assert_rnp_failure(rnp_input_from_path(NULL, "noexist"));
    assert_rnp_failure(rnp_input_from_path(&input, NULL));
    assert_rnp_failure(rnp_input_from_path(&input, "noexist"));
    assert_null(input);
    assert_rnp_failure(rnp_output_to_path(&output, ""));
    assert_null(output);
    assert_rnp_failure(rnp_output_to_path(NULL, "path"));
    assert_rnp_failure(rnp_output_to_path(&output, NULL));

    // create input+output
    assert_rnp_success(rnp_input_from_path(&input, "plaintext"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    assert_non_null(output);
    // create encrypt operation
    assert_rnp_failure(rnp_op_encrypt_create(NULL, ffi, input, output));
    assert_rnp_failure(rnp_op_encrypt_create(&op, NULL, input, output));
    assert_rnp_failure(rnp_op_encrypt_create(&op, ffi, NULL, output));
    assert_rnp_failure(rnp_op_encrypt_create(&op, ffi, input, NULL));
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    // add password (using all defaults)
    assert_rnp_failure(rnp_op_encrypt_add_password(NULL, "pass1", NULL, 0, NULL));
    assert_rnp_failure(rnp_op_encrypt_add_password(op, "", NULL, 0, NULL));
    assert_rnp_failure(rnp_op_encrypt_add_password(op, "pass1", "WRONG", 0, NULL));
    assert_rnp_failure(rnp_op_encrypt_add_password(op, "pass1", NULL, 0, "WRONG"));
    assert_rnp_success(rnp_op_encrypt_add_password(op, "pass1", NULL, 0, NULL));

    // Allow insecure ciphers
    if (blowfish_enabled()) {
        assert_rnp_success(rnp_remove_security_rule(
          ffi, RNP_FEATURE_SYMM_ALG, "BLOWFISH", 0, RNP_SECURITY_REMOVE_ALL, 0, nullptr));
    }
    if (cast5_enabled()) {
        assert_rnp_success(rnp_remove_security_rule(
          ffi, RNP_FEATURE_SYMM_ALG, "CAST5", 0, RNP_SECURITY_REMOVE_ALL, 0, nullptr));
    }

    // add password
    if (!sm2_enabled() && !twofish_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_add_password(op, "pass2", "SM3", 12345, "TWOFISH"));
        assert_rnp_failure(
          rnp_op_encrypt_add_password(op, "pass2", "SHA256", 12345, "TWOFISH"));
        const char *alg = blowfish_enabled() ? "BLOWFISH" : "AES256";
        assert_rnp_success(rnp_op_encrypt_add_password(op, "pass2", "SHA256", 12345, alg));
    } else if (!sm2_enabled() && twofish_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_add_password(op, "pass2", "SM3", 12345, "TWOFISH"));
        assert_rnp_success(
          rnp_op_encrypt_add_password(op, "pass2", "SHA256", 12345, "TWOFISH"));
    } else if (sm2_enabled() && !twofish_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_add_password(op, "pass2", "SM3", 12345, "TWOFISH"));
        const char *alg = blowfish_enabled() ? "BLOWFISH" : "AES256";
        assert_rnp_success(rnp_op_encrypt_add_password(op, "pass2", "SM3", 12345, alg));
    } else {
        assert_rnp_success(rnp_op_encrypt_add_password(op, "pass2", "SM3", 12345, "TWOFISH"));
    }
    // set the data encryption cipher
    assert_rnp_failure(rnp_op_encrypt_set_cipher(NULL, "CAST5"));
    assert_rnp_failure(rnp_op_encrypt_set_cipher(op, NULL));
    assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "WRONG"));
    if (cast5_enabled()) {
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "CAST5"));
    } else {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "CAST5"));
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "AES256"));
    }
    // execute the operation
    assert_rnp_failure(rnp_op_encrypt_execute(NULL));
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

    /* decrypt */

    // decrypt (no pass provider, should fail)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_failure(rnp_ffi_set_pass_provider(NULL, NULL, NULL));
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, NULL, NULL));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;

    // decrypt (wrong pass, should fail)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    const char *pass = "wrong1";
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, getpasscb_once, &pass));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;

    // decrypt (pass1)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "pass1"));
    assert_rnp_failure(rnp_decrypt(NULL, input, output));
    assert_rnp_failure(rnp_decrypt(ffi, NULL, output));
    assert_rnp_failure(rnp_decrypt(ffi, input, NULL));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;
    // compare the decrypted file
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    unlink("decrypted");

    // decrypt (pass2)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "pass2"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;
    // compare the decrypted file
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    // final cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_encrypt_pass_provider)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *plaintext = "Data encrypted with password provided via password provider.";

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // write out some data
    str_to_file("plaintext", plaintext);
    // create input + output
    assert_rnp_success(rnp_input_from_path(&input, "plaintext"));
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    // add password with NULL password provider set - should fail
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, NULL, NULL));
    assert_rnp_failure(rnp_op_encrypt_add_password(op, NULL, NULL, 0, NULL));
    // add password with password provider set.
    int pswdnum = 1;
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, getpasscb_inc, &pswdnum));
    assert_rnp_success(rnp_op_encrypt_add_password(op, NULL, NULL, 0, NULL));
    // add another password with different encryption parameters
    if (!sm2_enabled() && !twofish_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_add_password(op, NULL, "SM3", 12345, "TWOFISH"));
        assert_rnp_failure(rnp_op_encrypt_add_password(op, NULL, "SHA256", 12345, "TWOFISH"));
        assert_rnp_success(rnp_op_encrypt_add_password(op, NULL, NULL, 12345, NULL));
    } else if (!sm2_enabled() && twofish_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_add_password(op, NULL, "SM3", 12345, "TWOFISH"));
        assert_rnp_success(rnp_op_encrypt_add_password(op, NULL, "SHA256", 12345, "TWOFISH"));
    } else if (sm2_enabled() && !twofish_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_add_password(op, NULL, "SM3", 12345, "TWOFISH"));
        assert_rnp_success(rnp_op_encrypt_add_password(op, NULL, "SM3", 12345, NULL));
    } else {
        assert_rnp_success(rnp_op_encrypt_add_password(op, NULL, "SM3", 12345, "TWOFISH"));
    }
    // set the data encryption cipher
    assert_rnp_success(rnp_op_encrypt_set_cipher(op, "CAMELLIA256"));
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

    /* decrypt with pass1 */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "pass1"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    unlink("decrypted");

    /* decrypt with pass2 via provider */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    pswdnum = 2;
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, getpasscb_inc, &pswdnum));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    unlink("decrypted");

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_encrypt_set_cipher)
{
    /* setup FFI */
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* create input + output */
    rnp_input_t input = NULL;
    const char *plaintext = "Data encrypted with password using different CEK/KEK.";
    assert_rnp_failure(
      rnp_input_from_memory(NULL, (const uint8_t *) plaintext, strlen(plaintext), false));
    assert_rnp_failure(rnp_input_from_memory(&input, NULL, strlen(plaintext), false));
    assert_rnp_success(rnp_input_from_memory(&input, (const uint8_t *) plaintext, 0, false));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(
      rnp_input_from_memory(&input, (const uint8_t *) plaintext, strlen(plaintext), false));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    /* create encrypt operation */
    rnp_op_encrypt_t op = NULL;
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    /* use different sym algos */
    assert_rnp_success(rnp_op_encrypt_add_password(op, "password1", NULL, 0, "AES192"));
    assert_rnp_success(rnp_op_encrypt_add_password(op, "password2", NULL, 0, "AES128"));
    assert_rnp_success(rnp_op_encrypt_set_cipher(op, "AES256"));
    /* execute the operation */
    assert_rnp_success(rnp_op_encrypt_execute(op));
    assert_true(rnp_file_exists("encrypted"));
    /* cleanup */
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    /* decrypt with password1 */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password1"));
    rnp_op_verify_t verify;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_set_flags(NULL, RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT));
    assert_rnp_failure(rnp_op_verify_set_flags(verify, 0x77));
    assert_rnp_success(rnp_op_verify_set_flags(verify, RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT));
    assert_rnp_success(rnp_op_verify_set_flags(verify, 0));
    assert_rnp_success(rnp_op_verify_execute(verify));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    /* Check protection info */
    char *mode = NULL;
    char *cipher = NULL;
    bool  valid = false;
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "cfb-mdc");
    assert_string_equal(cipher, "AES256");
    assert_true(valid);
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    /* Check SESKs */
    size_t count = 0;
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 2);
    /* First SESK: AES192 */
    rnp_symenc_handle_t symenc = NULL;
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 0, &symenc));
    char *aalg = NULL;
    assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aalg));
    assert_string_equal(aalg, "None");
    assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
    assert_string_equal(cipher, "AES192");
    rnp_buffer_destroy(aalg);
    rnp_buffer_destroy(cipher);
    /* Second SESK: AES128 */
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 1, &symenc));
    assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aalg));
    assert_string_equal(aalg, "None");
    assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
    assert_string_equal(cipher, "AES128");
    rnp_buffer_destroy(aalg);
    rnp_buffer_destroy(cipher);
    unlink("decrypted");
    unlink("encrypted");
    rnp_op_verify_destroy(verify);

    /* Now use AEAD */
    assert_rnp_success(
      rnp_input_from_memory(&input, (const uint8_t *) plaintext, strlen(plaintext), false));
    assert_rnp_success(rnp_output_to_path(&output, "encrypted-aead"));
    /* create encrypt operation */
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    /* use different sym algos */
    assert_rnp_success(rnp_op_encrypt_add_password(op, "password1", NULL, 0, "AES256"));
    assert_rnp_success(rnp_op_encrypt_add_password(op, "password2", NULL, 0, "AES192"));
    assert_rnp_success(rnp_op_encrypt_set_cipher(op, "AES128"));
    assert_rnp_success(rnp_op_encrypt_set_aead(op, "OCB"));
    /* execute the operation */
    assert_rnp_success(rnp_op_encrypt_execute(op));
    assert_true(rnp_file_exists("encrypted-aead"));
    /* cleanup */
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    /* decrypt with password2 */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted-aead"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password2"));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    /* Check protection info */
    assert_rnp_success(rnp_op_verify_get_protection_info(verify, &mode, &cipher, &valid));
    assert_string_equal(mode, "aead-ocb");
    assert_string_equal(cipher, "AES128");
    assert_true(valid);
    rnp_buffer_destroy(mode);
    rnp_buffer_destroy(cipher);
    /* Check SESKs */
    assert_rnp_success(rnp_op_verify_get_symenc_count(verify, &count));
    assert_int_equal(count, 2);
    /* First SESK: AES192 */
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 0, &symenc));
    assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aalg));
    assert_string_equal(aalg, "OCB");
    assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
    assert_string_equal(cipher, "AES256");
    rnp_buffer_destroy(aalg);
    rnp_buffer_destroy(cipher);
    /* Second SESK: AES128 */
    assert_rnp_success(rnp_op_verify_get_symenc_at(verify, 1, &symenc));
    assert_rnp_success(rnp_symenc_get_aead_alg(symenc, &aalg));
    assert_string_equal(aalg, "OCB");
    assert_rnp_success(rnp_symenc_get_cipher(symenc, &cipher));
    assert_string_equal(cipher, "AES192");
    rnp_buffer_destroy(aalg);
    rnp_buffer_destroy(cipher);
    unlink("decrypted");
    unlink("encrypted-aead");
    rnp_op_verify_destroy(verify);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_encrypt_empty_buffer)
{
    /* setup FFI */
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* create input + output */
    rnp_input_t input = NULL;
    const char *plaintext = "";
    assert_rnp_success(
      rnp_input_from_memory(&input, (const uint8_t *) plaintext, strlen(plaintext), false));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    /* create encrypt operation */
    rnp_op_encrypt_t op = NULL;
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    assert_rnp_success(rnp_op_encrypt_add_password(op, "password1", NULL, 0, "AES192"));
    /* execute the operation */
    assert_rnp_success(rnp_op_encrypt_execute(op));
    assert_true(rnp_file_exists("encrypted"));
    /* cleanup */
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    /* decrypt with password1 */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password1"));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    unlink("decrypted");
    unlink("encrypted");

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_encrypt_null_buffer)
{
    /* setup FFI */
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* create input + output */
    rnp_input_t input = NULL;
    assert_rnp_failure(rnp_input_from_memory(&input, NULL, 10 /*not zero value*/, false));
    assert_rnp_success(rnp_input_from_memory(&input, NULL, 0, false));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    /* create encrypt operation */
    rnp_op_encrypt_t op = NULL;
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    assert_rnp_success(rnp_op_encrypt_add_password(op, "password1", NULL, 0, "AES192"));
    /* execute the operation */
    assert_rnp_success(rnp_op_encrypt_execute(op));
    assert_true(rnp_file_exists("encrypted"));
    /* cleanup */
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    /* decrypt with password1 */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password1"));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), "");
    unlink("decrypted");
    unlink("encrypted");

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_encrypt_pk)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *     plaintext = "data1";

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    // load our keyrings
    assert_true(
      load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring.gpg"));

    // write out some data
    str_to_file("plaintext", plaintext);
    // create input+output
    assert_rnp_success(rnp_input_from_path(&input, "plaintext"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    assert_non_null(output);
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    // add recipients
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key0-uid2", &key));
    assert_rnp_failure(rnp_op_encrypt_add_recipient(NULL, key));
    assert_rnp_failure(rnp_op_encrypt_add_recipient(op, NULL));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid1", &key));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    key = NULL;
    // set the data encryption cipher
    if (cast5_enabled()) {
        assert_rnp_success(rnp_remove_security_rule(
          ffi, RNP_FEATURE_SYMM_ALG, "CAST5", 0, RNP_SECURITY_REMOVE_ALL, 0, nullptr));
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "CAST5"));
    } else {
        assert_rnp_failure(rnp_remove_security_rule(
          ffi, RNP_FEATURE_SYMM_ALG, "CAST5", 0, RNP_SECURITY_REMOVE_ALL, 0, nullptr));
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "CAST5"));
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "AES256"));
    }
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

    /* decrypt */

    // decrypt (no pass provider, should fail)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, NULL, NULL));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;

    // decrypt (wrong pass, should fail)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    const char *pass = "wrong1";
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, getpasscb_once, &pass));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;

    // decrypt
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;
    // read in the decrypted file
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    // final cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_select_deprecated_ciphers)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *     plaintext = "data1";

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    assert_rnp_success(
      rnp_input_from_memory(&input, (const uint8_t *) plaintext, strlen(plaintext), false));
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));

    /* check predefined rules */
    uint32_t flags = 0;
    uint64_t from = 0;
    uint32_t level = 0;
    if (cast5_enabled()) {
        assert_rnp_success(rnp_get_security_rule(ffi,
                                                 RNP_FEATURE_SYMM_ALG,
                                                 "CAST5",
                                                 CAST5_3DES_IDEA_BLOWFISH_FROM + 1,
                                                 &flags,
                                                 &from,
                                                 &level));
        assert_int_equal(from, CAST5_3DES_IDEA_BLOWFISH_FROM);
        assert_int_equal(level, RNP_SECURITY_INSECURE);
    }

    assert_rnp_success(rnp_get_security_rule(ffi,
                                             RNP_FEATURE_SYMM_ALG,
                                             "TRIPLEDES",
                                             CAST5_3DES_IDEA_BLOWFISH_FROM + 1,
                                             &flags,
                                             &from,
                                             &level));
    assert_int_equal(from, CAST5_3DES_IDEA_BLOWFISH_FROM);
    assert_int_equal(level, RNP_SECURITY_INSECURE);
    if (idea_enabled()) {
        assert_rnp_success(rnp_get_security_rule(ffi,
                                                 RNP_FEATURE_SYMM_ALG,
                                                 "IDEA",
                                                 CAST5_3DES_IDEA_BLOWFISH_FROM + 1,
                                                 &flags,
                                                 &from,
                                                 &level));
        assert_int_equal(from, CAST5_3DES_IDEA_BLOWFISH_FROM);
        assert_int_equal(level, RNP_SECURITY_INSECURE);
    }
    if (blowfish_enabled()) {
        assert_rnp_success(rnp_get_security_rule(ffi,
                                                 RNP_FEATURE_SYMM_ALG,
                                                 "BLOWFISH",
                                                 CAST5_3DES_IDEA_BLOWFISH_FROM + 1,
                                                 &flags,
                                                 &from,
                                                 &level));
        assert_int_equal(from, CAST5_3DES_IDEA_BLOWFISH_FROM);
        assert_int_equal(level, RNP_SECURITY_INSECURE);
    }

    ffi->context.set_time(CAST5_3DES_IDEA_BLOWFISH_FROM + 1);
    // set the data encryption cipher
    if (cast5_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "CAST5"));
    }
    assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "TRIPLEDES"));
    if (idea_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "IDEA"));
    }
    if (blowfish_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "BLOWFISH"));
    }

    ffi->context.set_time(CAST5_3DES_IDEA_BLOWFISH_FROM - 1);

    if (cast5_enabled()) {
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "CAST5"));
    }
    assert_rnp_success(rnp_op_encrypt_set_cipher(op, "TRIPLEDES"));
    if (idea_enabled()) {
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "IDEA"));
    }
    if (blowfish_enabled()) {
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "BLOWFISH"));
    }

    // cleanup
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    op = NULL;
    rnp_ffi_destroy(ffi);

    // check again but with removing rules now
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));

    ffi->context.set_time(CAST5_3DES_IDEA_BLOWFISH_FROM + 1);
    // set the data encryption cipher
    if (cast5_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "CAST5"));
    }
    assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "TRIPLEDES"));
    if (idea_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "IDEA"));
    }
    if (blowfish_enabled()) {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "BLOWFISH"));
    }

    size_t removed = 0;
    if (cast5_enabled()) {
        removed = 0;
        assert_rnp_success(rnp_remove_security_rule(
          ffi, RNP_FEATURE_SYMM_ALG, "CAST5", 0, RNP_SECURITY_REMOVE_ALL, 0, &removed));
        assert_int_equal(removed, 1);
    }
    removed = 0;
    assert_rnp_success(rnp_remove_security_rule(
      ffi, RNP_FEATURE_SYMM_ALG, "TRIPLEDES", 0, RNP_SECURITY_REMOVE_ALL, 0, &removed));
    assert_int_equal(removed, 1);
    if (idea_enabled()) {
        removed = 0;
        assert_rnp_success(rnp_remove_security_rule(
          ffi, RNP_FEATURE_SYMM_ALG, "IDEA", 0, RNP_SECURITY_REMOVE_ALL, 0, &removed));
        assert_int_equal(removed, 1);
    }
    if (blowfish_enabled()) {
        removed = 0;
        assert_rnp_success(rnp_remove_security_rule(
          ffi, RNP_FEATURE_SYMM_ALG, "BLOWFISH", 0, RNP_SECURITY_REMOVE_ALL, 0, &removed));
        assert_int_equal(removed, 1);
    }

    if (cast5_enabled()) {
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "CAST5"));
    }
    assert_rnp_success(rnp_op_encrypt_set_cipher(op, "TRIPLEDES"));
    if (idea_enabled()) {
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "IDEA"));
    }
    if (blowfish_enabled()) {
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "BLOWFISH"));
    }

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    op = NULL;
    rnp_ffi_destroy(ffi);
}

static bool
first_key_password_provider(rnp_ffi_t        ffi,
                            void *           app_ctx,
                            rnp_key_handle_t key,
                            const char *     pgp_context,
                            char *           buf,
                            size_t           buf_len)
{
    if (!key) {
        throw std::invalid_argument("key");
    }
    char *keyid = NULL;
    rnp_key_get_keyid(key, &keyid);
    if (strcmp(keyid, "8A05B89FAD5ADED1")) {
        throw std::invalid_argument("keyid");
    }
    rnp_buffer_destroy(keyid);
    return false;
}

TEST_F(rnp_tests, test_ffi_decrypt_pk_unlocked)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *     plaintext = "data1";

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    // load our keyrings
    assert_true(
      load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring.gpg"));

    // write out some data
    str_to_file("plaintext", plaintext);
    // create input+output
    assert_rnp_success(rnp_input_from_path(&input, "plaintext"));
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    // add recipients
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key0-uid2", &key));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid1", &key));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    // execute the operation
    assert_rnp_success(rnp_op_encrypt_execute(op));

    // make sure the output file was created
    assert_true(rnp_file_exists("encrypted"));

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_op_encrypt_destroy(op));

    /* decrypt (unlocked first key, no pass provider) */
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, NULL, NULL));
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    rnp_key_handle_t defkey = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key0-uid2", &key));
    assert_rnp_success(rnp_key_get_default_key(key, "encrypt", 0, &defkey));
    assert_non_null(defkey);
    assert_rnp_success(rnp_key_unlock(defkey, "password"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    assert_rnp_success(rnp_key_lock(defkey));
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(defkey);
    // cleanup
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    assert_int_equal(unlink("decrypted"), 0);

    /* decrypt (unlocked second key, no pass provider) */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid1", &key));
    assert_rnp_success(rnp_key_get_default_key(key, "encrypt", 0, &defkey));
    assert_non_null(defkey);
    assert_rnp_success(rnp_key_unlock(defkey, "password"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    assert_rnp_success(rnp_key_lock(defkey));
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(defkey);
    // cleanup
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    assert_int_equal(unlink("decrypted"), 0);

    /* decrypt (unlocked first key, pass provider should not be called) */
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, ffi_asserting_password_provider, NULL));
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key0-uid2", &key));
    assert_rnp_success(rnp_key_get_default_key(key, "encrypt", 0, &defkey));
    assert_non_null(defkey);
    assert_rnp_success(rnp_key_unlock(defkey, "password"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    assert_rnp_success(rnp_key_lock(defkey));
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(defkey);
    // cleanup
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    assert_int_equal(unlink("decrypted"), 0);

    /* decrypt (unlocked second key, pass provider should not be called) */
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, first_key_password_provider, NULL));
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid1", &key));
    assert_rnp_success(rnp_key_get_default_key(key, "encrypt", 0, &defkey));
    assert_non_null(defkey);
    assert_rnp_success(rnp_key_unlock(defkey, "password"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    assert_rnp_success(rnp_key_lock(defkey));
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(defkey);
    // cleanup
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    assert_int_equal(unlink("decrypted"), 0);

    // final cleanup
    rnp_ffi_destroy(ffi);
}

#if defined(ENABLE_CRYPTO_REFRESH)
TEST_F(rnp_tests, test_ffi_decrypt_v6_pkesk_test_vector)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, "data/test_v6_valid_data/transferable_seckey_v6.asc"));

    assert_rnp_success(rnp_input_from_path(&input, "data/test_v6_valid_data/v6pkesk.asc"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_decrypt(ffi, input, output));

    // cleanup
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_ffi_destroy(ffi);
}

#if defined(ENABLE_PQC)
// NOTE: this tests ML-KEM-ipd test vectors
// The final implementation of the PQC draft implementation will use the final NIST standard.
TEST_F(rnp_tests, test_ffi_decrypt_pqc_pkesk_test_vector)
{
    bool expect_success = true;
#if !(defined(BOTAN_HAS_ML_KEM_INITIAL_PUBLIC_DRAFT) && defined(ENABLE_PQC_MLKEM_IPD))
    // we can only verify the test vectors with ML-KEM-ipd
    expect_success = false;
#endif

    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, "data/draft-ietf-openpgp-pqc/v6-eddsa-mlkem.sec.asc"));
    assert_true(import_all_keys(ffi, "data/draft-ietf-openpgp-pqc/v4-eddsa-mlkem.sec.asc"));

    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/draft-ietf-openpgp-pqc/v6-seipdv2.asc"));
    assert_non_null(input);
    if (expect_success) {
        assert_rnp_success(rnp_decrypt(ffi, input, output));
        assert_string_equal(file_to_str("decrypted").c_str(), "Testing\n");
    } else {
        assert_rnp_failure(rnp_decrypt(ffi, input, output));
    }
    assert_int_equal(unlink("decrypted"), 0);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/draft-ietf-openpgp-pqc/v4-seipdv1.asc"));
    assert_non_null(input);
    if (expect_success) {
        assert_rnp_success(rnp_decrypt(ffi, input, output));
        assert_string_equal(file_to_str("decrypted").c_str(), "Testing\n");
    } else {
        assert_rnp_failure(rnp_decrypt(ffi, input, output));
    }
    assert_int_equal(unlink("decrypted"), 0);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/draft-ietf-openpgp-pqc/v4-seipdv1.asc"));
    assert_non_null(input);
    if (expect_success) {
        assert_rnp_success(rnp_decrypt(ffi, input, output));
        assert_string_equal(file_to_str("decrypted").c_str(), "Testing\n");
    } else {
        assert_rnp_failure(rnp_decrypt(ffi, input, output));
    }
    assert_int_equal(unlink("decrypted"), 0);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_pqc_default_enc_subkey)
{
    rnp_ffi_t         ffi = NULL;
    rnp_key_handle_t  key1 = NULL;
    rnp_key_handle_t  key2 = NULL;
    rnp_key_handle_t  defkey1 = NULL;
    rnp_key_handle_t  defkey2 = NULL;
    rnp_op_generate_t op = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* generate key 1 */
    assert_rnp_success(rnp_op_generate_create(&op, ffi, "ML-DSA-65+ED25519"));
    assert_rnp_success(rnp_op_generate_set_hash(op, "SHA3-256"));
    assert_rnp_success(rnp_op_generate_execute(op));

    assert_rnp_success(rnp_op_generate_get_key(op, &key1));
    assert_non_null(key1);
    assert_rnp_success(rnp_op_generate_destroy(op));
    op = NULL;
    assert_rnp_success(rnp_op_generate_subkey_create(&op, ffi, key1, "ML-KEM-768+X25519"));
    assert_rnp_success(rnp_op_generate_execute(op));
    rnp_op_generate_destroy(op);
    op = NULL;
    assert_rnp_success(rnp_op_generate_subkey_create(&op, ffi, key1, "ECDH"));
    assert_rnp_success(rnp_op_generate_set_curve(op, "NIST P-256"));
    assert_rnp_success(rnp_op_generate_execute(op));
    rnp_op_generate_destroy(op);
    op = NULL;

    /* generate key 2 */
    assert_rnp_success(rnp_op_generate_create(&op, ffi, "ML-DSA-65+ED25519"));
    assert_rnp_success(rnp_op_generate_set_hash(op, "SHA3-256"));
    assert_rnp_success(rnp_op_generate_execute(op));
    assert_rnp_success(rnp_op_generate_get_key(op, &key2));
    assert_non_null(key2);
    assert_rnp_success(rnp_op_generate_destroy(op));
    op = NULL;
    assert_rnp_success(rnp_op_generate_subkey_create(&op, ffi, key2, "ECDH"));
    assert_rnp_success(rnp_op_generate_set_curve(op, "NIST P-256"));
    assert_rnp_success(rnp_op_generate_execute(op));
    rnp_op_generate_destroy(op);
    op = NULL;
    assert_rnp_success(rnp_op_generate_subkey_create(&op, ffi, key2, "ML-KEM-768+X25519"));
    assert_rnp_success(rnp_op_generate_execute(op));
    rnp_op_generate_destroy(op);
    op = NULL;

    /* check default key */
    assert_rnp_success(
      rnp_key_get_default_key(key1, "encrypt", RNP_KEY_PREFER_PQC_ENC_SUBKEY, &defkey1));
    // PQC key is older but preferred
    assert(defkey1->pub->alg() == PGP_PKA_KYBER768_X25519);
    assert_rnp_success(
      rnp_key_get_default_key(key2, "encrypt", RNP_KEY_PREFER_PQC_ENC_SUBKEY, &defkey2));
    // PQC key is newer and preferred
    assert(defkey2->pub->alg() == PGP_PKA_KYBER768_X25519);

    /* cleanup */
    rnp_key_handle_destroy(key1);
    rnp_key_handle_destroy(key2);
    rnp_key_handle_destroy(defkey1);
    rnp_key_handle_destroy(defkey2);
    rnp_ffi_destroy(ffi);
}
#endif

TEST_F(rnp_tests, test_ffi_encrypt_pk_with_v6_key)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *     plaintext = "data1";

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    assert_true(import_all_keys(ffi, "data/test_v6_valid_data/transferable_seckey_v6.asc"));

    std::vector<std::string> ciphers = {"AES128", "AES192", "AES256"};
    std::vector<std::string> aead_modes = {"None", "EAX", "OCB"};
    std::vector<bool>        enable_pkeskv6_modes = {true, false};

    for (auto enable_pkeskv6 : enable_pkeskv6_modes)
        for (auto aead : aead_modes)
            for (auto cipher : ciphers) {
                // write out some data
                FILE *fp = fopen("plaintext", "wb");
                assert_non_null(fp);
                assert_int_equal(1, fwrite(plaintext, strlen(plaintext), 1, fp));
                assert_int_equal(0, fclose(fp));

                // create input+output
                assert_rnp_success(rnp_input_from_path(&input, "plaintext"));
                assert_non_null(input);
                assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
                assert_non_null(output);
                // create encrypt operation
                assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
                // add recipients
                rnp_key_handle_t key = NULL;
                assert_rnp_success(rnp_locate_key(ffi, "keyid", "12c83f1e706f6308", &key));
                assert_non_null(key);

                assert_rnp_failure(rnp_op_encrypt_add_recipient(op, NULL)); // what for ?
                assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
                if (enable_pkeskv6) {
                    assert_rnp_success(rnp_op_encrypt_enable_pkesk_v6(op));
                }
                rnp_key_handle_destroy(key);
                key = NULL;

                // set the data encryption cipher
                if ((aead == "None") && enable_pkeskv6) {
                    // already enabled v6 pkesk, does not make any sense to set AEAD to None
                    // explicitly.
                    assert_rnp_failure(rnp_op_encrypt_set_aead(op, aead.c_str()));
                } else {
                    assert_rnp_success(rnp_op_encrypt_set_aead(op, aead.c_str()));
                }
                assert_rnp_success(rnp_op_encrypt_set_cipher(op, cipher.c_str()));

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

                /* decrypt */

                // decrypt
                assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
                assert_non_null(input);
                assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
                assert_non_null(output);
                assert_rnp_success(rnp_ffi_set_pass_provider(ffi, NULL, NULL));
                assert_rnp_success(rnp_decrypt(ffi, input, output));
                // cleanup
                rnp_input_destroy(input);
                input = NULL;
                rnp_output_destroy(output);
                output = NULL;
            }
    rnp_ffi_destroy(ffi);
}
#endif

TEST_F(rnp_tests, test_ffi_encrypt_pk_key_provider)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *     plaintext = "data1";
    uint8_t *        primary_sec_key_data = NULL;
    size_t           primary_sec_size = 0;
    uint8_t *        sub_sec_key_data = NULL;
    size_t           sub_sec_size = 0;

    /* first, let's generate some encrypted data */
    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_non_null(ffi);
    // load our keyrings
    assert_true(
      load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring.gpg"));
    // write out some data
    str_to_file("plaintext", plaintext);
    // create input+output
    assert_rnp_success(rnp_input_from_path(&input, "plaintext"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    assert_non_null(output);
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    // add recipient 1
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key0-uid2", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    // cleanup
    assert_rnp_success(rnp_key_handle_destroy(key));
    key = NULL;
    // add recipient 2
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid1", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    // save the primary key data for later
    assert_rnp_success(rnp_get_secret_key_data(key, &primary_sec_key_data, &primary_sec_size));
    assert_non_null(primary_sec_key_data);
    assert_rnp_success(rnp_key_handle_destroy(key));
    key = NULL;
    // save the appropriate encrypting subkey for the key provider to use during decryption
    // later
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "8A05B89FAD5ADED1", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_get_secret_key_data(key, &sub_sec_key_data, &sub_sec_size));
    assert_non_null(sub_sec_key_data);
    // cleanup
    assert_rnp_success(rnp_key_handle_destroy(key));
    key = NULL;
    // set the data encryption cipher
    if (cast5_enabled()) {
        if (cast5_enabled()) {
            assert_rnp_success(rnp_remove_security_rule(
              ffi, RNP_FEATURE_SYMM_ALG, "CAST5", 0, RNP_SECURITY_REMOVE_ALL, 0, NULL));
        }
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "CAST5"));
    } else {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "CAST5"));
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "AES256"));
    }
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
    assert_rnp_success(rnp_ffi_destroy(ffi));
    ffi = NULL;

    /* decrypt */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load the primary
    input = NULL;
    assert_rnp_success(
      rnp_input_from_memory(&input, primary_sec_key_data, primary_sec_size, true));
    assert_non_null(input);
    assert_rnp_success(rnp_load_keys(ffi, "GPG", input, RNP_LOAD_SAVE_SECRET_KEYS));
    rnp_input_destroy(input);
    input = NULL;

    // decrypt (no key to decrypt, should fail)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_int_equal(RNP_ERROR_NO_SUITABLE_KEY, rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;

    // key_data key_data_size secret keyid grip userids
    const key_tbl_t keydb[] = {
      {sub_sec_key_data, sub_sec_size, true, "8A05B89FAD5ADED1", NULL, {NULL}}, {0}};

    // decrypt
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_success(rnp_ffi_set_key_provider(ffi, tbl_getkeycb, (void *) keydb));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;
    // compare the decrypted file
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    // final cleanup
    rnp_ffi_destroy(ffi);
    free(sub_sec_key_data);
    free(primary_sec_key_data);
}

TEST_F(rnp_tests, test_ffi_encrypt_and_sign)
{
    rnp_ffi_t               ffi = NULL;
    rnp_input_t             input = NULL;
    rnp_output_t            output = NULL;
    rnp_op_encrypt_t        op = NULL;
    rnp_op_sign_signature_t signsig = NULL;
    const char *            plaintext = "data1";
    rnp_key_handle_t        key = NULL;
    const uint32_t          issued = 1516211899;   // Unix epoch, nowish
    const uint32_t          expires = 1000000000;  // expires later
    const uint32_t          issued2 = 1516211900;  // Unix epoch, nowish
    const uint32_t          expires2 = 2000000000; // expires later

    // setup FFI
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    // load our keyrings
    assert_true(
      load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring.gpg"));

    // write out some data
    str_to_file("plaintext", plaintext);
    // create input+output
    assert_rnp_success(rnp_input_from_path(&input, "plaintext"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    assert_non_null(output);
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    // add recipients
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key0-uid2", &key));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid1", &key));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    key = NULL;
    // set the data encryption cipher
    if (cast5_enabled()) {
        if (cast5_enabled()) {
            assert_rnp_success(rnp_remove_security_rule(
              ffi, RNP_FEATURE_SYMM_ALG, "CAST5", 0, RNP_SECURITY_REMOVE_ALL, 0, NULL));
        }
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "CAST5"));
    } else {
        assert_rnp_failure(rnp_op_encrypt_set_cipher(op, "CAST5"));
        assert_rnp_success(rnp_op_encrypt_set_cipher(op, "AES256"));
    }
    // enable armoring
    assert_rnp_failure(rnp_op_encrypt_set_armor(NULL, true));
    assert_rnp_success(rnp_op_encrypt_set_armor(op, true));
    // add signature
    assert_rnp_failure(rnp_op_encrypt_set_hash(NULL, "SHA256"));
    assert_rnp_failure(rnp_op_encrypt_set_hash(op, NULL));
    assert_rnp_failure(rnp_op_encrypt_set_hash(op, "WRONG"));
    assert_rnp_success(rnp_op_encrypt_set_hash(op, "SHA1"));
    assert_rnp_failure(rnp_op_encrypt_set_creation_time(NULL, 0));
    assert_rnp_success(rnp_op_encrypt_set_creation_time(op, 0));
    assert_rnp_failure(rnp_op_encrypt_set_expiration_time(NULL, 0));
    assert_rnp_success(rnp_op_encrypt_set_expiration_time(op, 0));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid1", &key));
    assert_rnp_failure(rnp_op_encrypt_add_signature(NULL, key, NULL));
    assert_rnp_failure(rnp_op_encrypt_add_signature(op, NULL, NULL));
    assert_rnp_success(rnp_op_encrypt_add_signature(op, key, NULL));
    rnp_key_handle_destroy(key);
    key = NULL;
    // attempt to add signature from the public key
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/ecc-p256-pub.asc"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    assert_rnp_failure(rnp_op_encrypt_add_signature(op, key, &signsig));
    rnp_key_handle_destroy(key);
    key = NULL;
    // attempt to add signature by the offline secret key
    assert_true(
      import_pub_keys(ffi, "data/test_key_edge_cases/alice-s2k-101-no-sign-sub.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    assert_rnp_failure(rnp_op_encrypt_add_signature(op, key, &signsig));
    rnp_key_handle_destroy(key);
    key = NULL;
    // add second signature with different hash/issued/expiration
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key1-uid2", &key));
    assert_rnp_success(rnp_op_encrypt_add_signature(op, key, &signsig));
    assert_rnp_failure(rnp_op_sign_signature_set_creation_time(NULL, issued2));
    assert_rnp_success(rnp_op_sign_signature_set_creation_time(signsig, issued2));
    assert_rnp_failure(rnp_op_sign_signature_set_expiration_time(NULL, expires2));
    assert_rnp_success(rnp_op_sign_signature_set_expiration_time(signsig, expires2));
    assert_rnp_failure(rnp_op_sign_signature_set_hash(signsig, NULL));
    assert_rnp_failure(rnp_op_sign_signature_set_hash(NULL, "SHA512"));
    assert_rnp_failure(rnp_op_sign_signature_set_hash(signsig, "UNKNOWN"));
    assert_rnp_success(rnp_op_sign_signature_set_hash(signsig, "SHA512"));
    rnp_key_handle_destroy(key);
    key = NULL;
    // set default sig parameters after the signature is added - those should be picked up
    assert_rnp_success(rnp_op_encrypt_set_hash(op, "SHA256"));
    assert_rnp_success(rnp_op_encrypt_set_creation_time(op, issued));
    assert_rnp_success(rnp_op_encrypt_set_expiration_time(op, expires));
    // execute the operation
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_op_encrypt_execute(op));

    // make sure the output file was created
    assert_true(rnp_file_exists("encrypted"));

    // check whether keys are locked
    rnp_identifier_iterator_t it = NULL;
    assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "fingerprint"));
    const char *fprint = NULL;
    while (!rnp_identifier_iterator_next(it, &fprint)) {
        if (!fprint) {
            break;
        }
        SCOPED_TRACE(fprint);
        rnp_key_handle_t skey = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "fingerprint", fprint, &skey));
        bool secret = true;
        assert_rnp_success(rnp_key_have_secret(skey, &secret));
        if (secret) {
            bool locked = false;
            assert_rnp_success(rnp_key_is_locked(skey, &locked));
            assert_true(locked);
        }
        rnp_key_handle_destroy(skey);
    }
    rnp_identifier_iterator_destroy(it);

    // cleanup
    assert_rnp_success(rnp_input_destroy(input));
    input = NULL;
    assert_rnp_success(rnp_output_destroy(output));
    output = NULL;
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    op = NULL;

    /* decrypt */

    // decrypt (no pass provider, should fail)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, NULL, NULL));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;

    // decrypt (wrong pass, should fail)
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    const char *pass = "wrong1";
    assert_rnp_success(rnp_ffi_set_pass_provider(ffi, getpasscb_once, &pass));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;

    // decrypt
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_non_null(output);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    // cleanup
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;
    // compare the decrypted file
    assert_string_equal(file_to_str("decrypted").c_str(), plaintext);
    // verify and check signatures
    rnp_op_verify_t verify;
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_non_null(input);
    assert_rnp_success(rnp_output_to_path(&output, "verified"));
    assert_non_null(output);
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    // check signatures
    rnp_op_verify_signature_t sig;
    size_t                    sig_count;
    uint32_t                  sig_create;
    uint32_t                  sig_expires;
    char *                    hname = NULL;

    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sig_count));
    assert_int_equal(sig_count, 2);
    assert_rnp_failure(rnp_op_verify_get_signature_at(NULL, 0, &sig));
    assert_rnp_failure(rnp_op_verify_get_signature_at(verify, 0, NULL));
    assert_rnp_failure(rnp_op_verify_get_signature_at(verify, 10, &sig));
    // signature 1
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_failure(rnp_op_verify_signature_get_status(NULL));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    assert_rnp_success(rnp_op_verify_signature_get_times(sig, &sig_create, &sig_expires));
    assert_int_equal(sig_create, issued);
    assert_int_equal(sig_expires, expires);
    assert_rnp_failure(rnp_op_verify_signature_get_hash(NULL, &hname));
    assert_rnp_failure(rnp_op_verify_signature_get_hash(sig, NULL));
    assert_rnp_success(rnp_op_verify_signature_get_hash(sig, &hname));
    assert_string_equal(hname, "SHA256");
    rnp_buffer_destroy(hname);
    hname = NULL;
    key = NULL;
    assert_rnp_failure(rnp_op_verify_signature_get_key(NULL, &key));
    assert_rnp_failure(rnp_op_verify_signature_get_key(sig, NULL));
    assert_rnp_success(rnp_op_verify_signature_get_key(sig, &key));
    assert_non_null(key);
    rnp_key_handle_destroy(key);
    // signature 2
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 1, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    assert_rnp_success(rnp_op_verify_signature_get_times(sig, &sig_create, &sig_expires));
    assert_int_equal(sig_create, issued2);
    assert_int_equal(sig_expires, expires2);
    assert_rnp_success(rnp_op_verify_signature_get_hash(sig, &hname));
    assert_string_equal(hname, "SHA512");
    rnp_buffer_destroy(hname);
    hname = NULL;
    // make sure keys are locked
    assert_rnp_success(rnp_identifier_iterator_create(ffi, &it, "fingerprint"));
    while (!rnp_identifier_iterator_next(it, &fprint)) {
        if (!fprint) {
            break;
        }
        SCOPED_TRACE(fprint);
        rnp_key_handle_t skey = NULL;
        assert_rnp_success(rnp_locate_key(ffi, "fingerprint", fprint, &skey));
        bool secret = true;
        assert_rnp_success(rnp_key_have_secret(skey, &secret));
        if (secret) {
            bool locked = false;
            assert_rnp_success(rnp_key_is_locked(skey, &locked));
            assert_true(locked);
        }
        rnp_key_handle_destroy(skey);
    }
    rnp_identifier_iterator_destroy(it);
    // cleanup
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    input = NULL;
    rnp_output_destroy(output);
    output = NULL;
    // compare the decrypted file
    assert_string_equal(file_to_str("verified").c_str(), plaintext);
    // final cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_encrypt_pk_subkey_selection)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *     plaintext = "data1";

    /* check whether a latest subkey is selected for encryption */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));

    /* case 1: three encryption subkeys, second expired, third has later creation time */
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/key0-sub02.pgp"));

    assert_rnp_success(
      rnp_input_from_memory(&input, (uint8_t *) plaintext, strlen(plaintext), false));
    assert_rnp_failure(rnp_output_to_memory(NULL, 0));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    /* create encrypt operation, add recipient and execute */
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key0-uid0", &key));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_op_encrypt_execute(op));
    /* get output */
    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, true));
    assert_true(buf && len);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_op_encrypt_destroy(op);
    /* decrypt */
    assert_true(load_keys_gpg(ffi, "", "data/keyrings/1/secring.gpg"));
    assert_rnp_success(rnp_input_from_memory(&input, buf, len, true));
    rnp_buffer_destroy(buf);
    assert_rnp_success(rnp_output_to_memory(&output, 0));

    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));

    /* check whether we used correct subkey */
    size_t count = 0;
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 1);
    rnp_recipient_handle_t recipient = NULL;
    assert_rnp_success(rnp_op_verify_get_recipient_at(verify, 0, &recipient));
    assert_non_null(recipient);
    char *keyid = NULL;
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "8A05B89FAD5ADED1");
    rnp_buffer_destroy(keyid);

    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* case 2: only subkeys 1-2, make sure that latest but expired subkey is not selected */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/key0-sub01.pgp"));
    assert_rnp_success(
      rnp_input_from_memory(&input, (uint8_t *) plaintext, strlen(plaintext), false));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    /* create encrypt operation, add recipient and execute */
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_op_encrypt_execute(op));
    /* get output */
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, true));
    assert_true(buf && len);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_op_encrypt_destroy(op);
    /* decrypt */
    assert_true(load_keys_gpg(ffi, "", "data/keyrings/1/secring.gpg"));
    assert_rnp_success(rnp_input_from_memory(&input, buf, len, true));
    rnp_buffer_destroy(buf);
    assert_rnp_success(rnp_output_to_memory(&output, 0));

    verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));

    /* check whether we used correct subkey */
    count = 0;
    assert_rnp_success(rnp_op_verify_get_recipient_count(verify, &count));
    assert_int_equal(count, 1);
    recipient = NULL;
    assert_rnp_success(rnp_op_verify_get_recipient_at(verify, 0, &recipient));
    assert_non_null(recipient);
    keyid = NULL;
    assert_rnp_success(rnp_recipient_get_keyid(recipient, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "1ED63EE56FADC34D");
    rnp_buffer_destroy(keyid);

    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* case 3: only expired subkey, make sure encryption operation fails */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/key0-sub1.pgp"));

    assert_rnp_success(
      rnp_input_from_memory(&input, (uint8_t *) plaintext, strlen(plaintext), false));
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    /* create encrypt operation, add recipient and execute */
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7bc6709b15c23a4a", &key));
    assert_int_equal(rnp_op_encrypt_add_recipient(op, key), RNP_ERROR_NO_SUITABLE_KEY);
    rnp_key_handle_destroy(key);
    assert_rnp_failure(rnp_op_encrypt_execute(op));
    rnp_op_encrypt_destroy(op);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_decrypt_small_rsa)
{
    rnp_ffi_t   ffi = NULL;
    const char *plaintext = "data1";

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, "data/test_key_validity/rsa_key_small_sig-sec.asc"));
    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/data.enc.small-rsa"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    size_t   len = 0;
    uint8_t *buf = NULL;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 5);
    assert_int_equal(memcmp(plaintext, buf, 5), 0);
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_decrypt_small_eg)
{
    /* make sure unlock and decrypt fails with invalid key */
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(
      import_all_keys(ffi, "data/test_key_edge_cases/key-eg-small-subgroup-sec.pgp"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "3b8dda452b9f69b4", &key));
    assert_non_null(key);
    /* key is not encrypted */
    assert_rnp_success(rnp_key_unlock(key, NULL));
    rnp_key_handle_destroy(key);
    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-eg-bad"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    rnp_ffi_destroy(ffi);
    /* make sure unlock and decrypt fails with invalid encrypted key */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_true(
      import_all_keys(ffi, "data/test_key_edge_cases/key-eg-small-subgroup-sec-enc.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "3b072c3bb2d1a8b2", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_unlock(key, "password"));
    assert_rnp_success(rnp_key_lock(key));
    rnp_key_handle_destroy(key);
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.enc-eg-bad2"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_encrypt_no_wrap)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(
      load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg", "data/keyrings/1/secring.gpg"));

    rnp_input_t input = NULL;
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt.signed"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_path(&output, "encrypted"));
    rnp_op_encrypt_t op = NULL;
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "key0-uid2", &key));
    assert_rnp_success(rnp_op_encrypt_add_recipient(op, key));
    rnp_key_handle_destroy(key);
    /* set nowrap flag */
    assert_rnp_failure(rnp_op_encrypt_set_flags(NULL, RNP_ENCRYPT_NOWRAP));
    assert_rnp_failure(rnp_op_encrypt_set_flags(op, 17));
    assert_rnp_success(rnp_op_encrypt_set_flags(op, RNP_ENCRYPT_NOWRAP));
    assert_rnp_success(rnp_op_encrypt_execute(op));

    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_op_encrypt_destroy(op));

    /* decrypt via rnp_decrypt() */
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "decrypted"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    assert_string_equal(file_to_str("decrypted").c_str(),
                        file_to_str("data/test_messages/message.txt").c_str());
    unlink("decrypted");

    /* decrypt and verify signatures */
    rnp_op_verify_t verify;
    assert_rnp_success(rnp_input_from_path(&input, "encrypted"));
    assert_rnp_success(rnp_output_to_path(&output, "verified"));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    /* check signature */
    rnp_op_verify_signature_t sig;
    size_t                    sig_count;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sig_count));
    assert_int_equal(sig_count, 1);
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    rnp_op_verify_destroy(verify);
    assert_string_equal(file_to_str("verified").c_str(),
                        file_to_str("data/test_messages/message.txt").c_str());
    unlink("verified");

    // final cleanup
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_v5_signatures)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* import v5 keys */
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/v5-rsa-pub.asc"));
    /* verify v5 attached signature */
    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.v5-rsa"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    rnp_op_verify_signature_t sig = NULL;
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    /* verify v5 detached signature */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_messages/message.txt"));
    rnp_input_t sigin = NULL;
    assert_rnp_success(
      rnp_input_from_path(&sigin, "data/test_messages/message.txt.signed.v5-detached-rsa"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, input, sigin));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_input_destroy(sigin);
    /* verify cleartext signature */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.v5-clear-rsa"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_mimemode_signature)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/ecc-25519-pub.asc"));

    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed-mimemode"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    size_t sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    rnp_op_verify_signature_t sig = NULL;
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_rnp_success(rnp_op_verify_signature_get_status(sig));
    char format = 0;
    assert_rnp_success(rnp_op_verify_get_format(verify, &format));
    assert_int_equal(format, 'm');
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_ffi_destroy(ffi);
}
