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

#include <sstream>
#include <rnp/rnp.h>
#include <librepgp/stream-ctx.h>
#include "key.hpp"
#include "ffi-priv-types.h"
#include "rnp_tests.h"
#include "support.h"

static bool check_sig_status(json_object *sig,
                             const char * pub,
                             const char * sec,
                             const char * fp);

TEST_F(rnp_tests, test_ffi_key_signatures)
{
    rnp_ffi_t ffi = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // load key
    assert_true(load_keys_gpg(ffi, "data/test_stream_key_load/ecc-p384-pub.asc"));
    // check primary key
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "242A3AA5EA85F44A", &key));
    // some edge cases
    size_t                 sigs = 0;
    rnp_signature_handle_t sig = NULL;
    assert_rnp_failure(rnp_key_get_signature_count(NULL, &sigs));
    assert_rnp_failure(rnp_key_get_signature_count(key, NULL));
    assert_rnp_failure(rnp_key_get_signature_at(key, 0, &sig));
    assert_rnp_failure(rnp_key_get_signature_at(key, 0x10000, &sig));
    assert_rnp_failure(rnp_key_get_signature_at(NULL, 0x10000, &sig));
    assert_rnp_failure(rnp_key_get_signature_at(NULL, 0, NULL));
    // key doesn't have signatures
    assert_rnp_success(rnp_key_get_signature_count(key, &sigs));
    assert_int_equal(sigs, 0);
    // uid must have one signature
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_get_signature_count(uid, &sigs));
    assert_int_equal(sigs, 1);
    assert_rnp_failure(rnp_uid_get_signature_at(uid, 1, &sig));
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    char *type = NULL;
    assert_rnp_failure(rnp_signature_get_type(NULL, &type));
    assert_rnp_failure(rnp_signature_get_type(sig, NULL));
    assert_rnp_success(rnp_signature_get_type(sig, &type));
    assert_string_equal(type, "certification (positive)");
    rnp_buffer_destroy(type);
    uint32_t creation = 0;
    assert_rnp_success(rnp_signature_get_creation(sig, &creation));
    assert_int_equal(creation, 1549119505);
    char *alg = NULL;
    assert_rnp_failure(rnp_signature_get_alg(NULL, &alg));
    assert_rnp_failure(rnp_signature_get_alg(sig, NULL));
    assert_rnp_success(rnp_signature_get_alg(sig, &alg));
    assert_string_equal(alg, "ECDSA");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_hash_alg(sig, &alg));
    assert_string_equal(alg, "SHA384");
    rnp_buffer_destroy(alg);
    char *keyid = NULL;
    assert_rnp_success(rnp_signature_get_keyid(sig, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "242A3AA5EA85F44A");
    rnp_buffer_destroy(keyid);
    char *keyfp = NULL;
    assert_rnp_failure(rnp_signature_get_key_fprint(sig, NULL));
    assert_rnp_failure(rnp_signature_get_key_fprint(NULL, &keyfp));
    assert_null(keyfp);
    assert_rnp_success(rnp_signature_get_key_fprint(sig, &keyfp));
    assert_string_equal(keyfp, "AB25CBA042DD924C3ACC3ED3242A3AA5EA85F44A");
    rnp_buffer_destroy(keyfp);
    rnp_key_handle_t signer = NULL;
    assert_rnp_success(rnp_signature_get_signer(sig, &signer));
    assert_non_null(signer);
    assert_rnp_success(rnp_key_get_keyid(signer, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "242A3AA5EA85F44A");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(signer);
    assert_int_equal(rnp_signature_is_valid(NULL, 0), RNP_ERROR_NULL_POINTER);
    assert_int_equal(rnp_signature_is_valid(sig, 17), RNP_ERROR_BAD_PARAMETERS);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    size_t errors = 100;
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 0);
    uint32_t error = 0;
    assert_int_equal(rnp_signature_error_at(sig, 0, &error), RNP_ERROR_BAD_PARAMETERS);
    assert_rnp_success(rnp_signature_handle_destroy(sig));
    // subkey must have one signature
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &subkey));
    assert_rnp_success(rnp_key_get_signature_count(subkey, &sigs));
    assert_int_equal(sigs, 1);
    assert_rnp_success(rnp_key_get_signature_at(subkey, 0, &sig));
    // check signature export
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_failure(rnp_signature_export(NULL, output, 0));
    assert_rnp_failure(rnp_signature_export(sig, NULL, 0));
    assert_rnp_failure(rnp_signature_export(sig, output, 0x333));
    assert_rnp_success(rnp_signature_export(sig, output, 0));
    uint8_t *buf = NULL;
    size_t   len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 154);
    rnp_input_t input;
    assert_rnp_success(rnp_input_from_memory(&input, buf, len, false));
    char *json = NULL;
    assert_rnp_success(rnp_import_signatures(ffi, input, 0, &json));
    assert_non_null(json);
    json_object *jso = json_tokener_parse(json);
    assert_non_null(jso);
    assert_true(json_object_is_type(jso, json_type_object));
    json_object *jsigs = NULL;
    assert_true(json_object_object_get_ex(jso, "sigs", &jsigs));
    assert_true(json_object_is_type(jsigs, json_type_array));
    assert_int_equal(json_object_array_length(jsigs), 1);
    json_object *jsig = json_object_array_get_idx(jsigs, 0);
    assert_true(check_sig_status(jsig, "none", "none", NULL));
    json_object_put(jso);
    rnp_buffer_destroy(json);
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));

    output = NULL;
    assert_rnp_success(rnp_output_to_memory(&output, 0));
    assert_rnp_success(rnp_signature_export(sig, output, RNP_KEY_EXPORT_ARMORED));
    buf = NULL;
    len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &len, false));
    assert_int_equal(len, 297);
    std::string data((const char *) buf, len);
    assert_true(starts_with(data, "-----BEGIN PGP PUBLIC KEY BLOCK-----"));
    assert_true(ends_with(strip_eol(data), "-----END PGP PUBLIC KEY BLOCK-----"));

    assert_rnp_success(rnp_input_from_memory(&input, buf, len, false));
    assert_rnp_success(rnp_import_signatures(ffi, input, 0, NULL));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));

    assert_rnp_success(rnp_signature_get_type(sig, &type));
    assert_string_equal(type, "subkey binding");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_signature_get_creation(sig, &creation));
    assert_int_equal(creation, 1549119513);
    assert_rnp_success(rnp_signature_get_alg(sig, &alg));
    assert_string_equal(alg, "ECDSA");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_hash_alg(sig, &alg));
    assert_string_equal(alg, "SHA384");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_keyid(sig, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "242A3AA5EA85F44A");
    rnp_buffer_destroy(keyid);
    assert_rnp_success(rnp_signature_get_signer(sig, &signer));
    assert_non_null(signer);
    assert_rnp_success(rnp_key_get_keyid(signer, &keyid));
    assert_non_null(keyid);
    assert_string_equal(keyid, "242A3AA5EA85F44A");
    rnp_buffer_destroy(keyid);
    rnp_key_handle_destroy(signer);
    rnp_key_handle_destroy(subkey);
    assert_rnp_success(rnp_signature_handle_destroy(sig));
    assert_rnp_success(rnp_uid_handle_destroy(uid));
    assert_rnp_success(rnp_key_handle_destroy(key));

    // check subkey which signature doesn't have issue fingerprint subpacket
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "326EF111425D14A5", &subkey));
    assert_rnp_success(rnp_key_get_signature_count(subkey, &sigs));
    assert_int_equal(sigs, 1);
    assert_rnp_success(rnp_key_get_signature_at(subkey, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &type));
    assert_string_equal(type, "subkey binding");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_signature_get_key_fprint(sig, &keyfp));
    assert_null(keyfp);
    rnp_signature_handle_destroy(sig);
    rnp_key_handle_destroy(subkey);

    // cleanup
    rnp_ffi_destroy(ffi);
}

static bool
check_import_sigs(rnp_ffi_t ffi, json_object **jso, json_object **sigarr, const char *sigpath)
{
    rnp_input_t input = NULL;
    if (rnp_input_from_path(&input, sigpath)) {
        return false;
    }
    bool  res = false;
    char *sigs = NULL;
    *jso = NULL;

    if (rnp_import_signatures(ffi, input, 0, &sigs)) {
        goto done;
    }
    if (!sigs) {
        goto done;
    }

    *jso = json_tokener_parse(sigs);
    if (!jso) {
        goto done;
    }
    if (!json_object_is_type(*jso, json_type_object)) {
        goto done;
    }
    if (!json_object_object_get_ex(*jso, "sigs", sigarr)) {
        goto done;
    }
    if (!json_object_is_type(*sigarr, json_type_array)) {
        goto done;
    }
    res = true;
done:
    if (!res) {
        json_object_put(*jso);
        *jso = NULL;
    }
    rnp_input_destroy(input);
    rnp_buffer_destroy(sigs);
    return res;
}

static bool
check_sig_status(json_object *sig, const char *pub, const char *sec, const char *fp)
{
    if (!sig) {
        return false;
    }
    if (!json_object_is_type(sig, json_type_object)) {
        return false;
    }
    json_object *fld = NULL;
    if (!json_object_object_get_ex(sig, "public", &fld)) {
        return false;
    }
    if (strcmp(json_object_get_string(fld), pub) != 0) {
        return false;
    }
    if (!json_object_object_get_ex(sig, "secret", &fld)) {
        return false;
    }
    if (strcmp(json_object_get_string(fld), sec) != 0) {
        return false;
    }
    if (!fp && json_object_object_get_ex(sig, "signer fingerprint", &fld)) {
        return false;
    }
    if (fp) {
        if (!json_object_object_get_ex(sig, "signer fingerprint", &fld)) {
            return false;
        }
        if (strcmp(json_object_get_string(fld), fp) != 0) {
            return false;
        }
    }
    return true;
}

TEST_F(rnp_tests, test_ffi_import_signatures)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t input = NULL;
    char *      results = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-pub.asc"));
    /* find key and check signature count */
    rnp_key_handle_t key_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    size_t sigcount = 0;
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 0);
    /* check revocation status */
    bool revoked = false;
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_false(revoked);
    /* some import edge cases */
    assert_rnp_failure(rnp_import_signatures(ffi, NULL, 0, &results));
    /* import revocation signature */
    json_object *jso = NULL;
    json_object *jsosigs = NULL;
    assert_true(
      check_import_sigs(ffi, &jso, &jsosigs, "data/test_key_validity/alice-rev.pgp"));
    assert_int_equal(json_object_array_length(jsosigs), 1);
    json_object *jsosig = json_object_array_get_idx(jsosigs, 0);
    assert_true(check_sig_status(
      jsosig, "new", "unknown key", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    json_object_put(jso);
    /* key now must become revoked */
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    /* check signature number - it now must be 1 */
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 1);
    /* check signature type */
    rnp_signature_handle_t sig = NULL;
    assert_rnp_success(rnp_key_get_signature_at(key_handle, 0, &sig));
    char *type = NULL;
    assert_rnp_success(rnp_signature_get_type(sig, &type));
    assert_string_equal(type, "key revocation");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    uint32_t screate = 0;
    assert_rnp_success(rnp_signature_get_creation(sig, &screate));
    assert_int_equal(screate, 1578663151);
    rnp_signature_handle_destroy(sig);
    /* check key validity */
    bool valid = true;
    assert_rnp_success(rnp_key_is_valid(key_handle, &valid));
    assert_false(valid);
    uint32_t till = 0;
    assert_rnp_success(rnp_key_valid_till(key_handle, &till));
    assert_int_equal(till, 1578663151);
    /* check import with NULL results param */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_key_validity/alice-rev.pgp"));
    assert_rnp_failure(rnp_import_signatures(NULL, input, 0, &results));
    assert_rnp_failure(rnp_import_signatures(ffi, input, 0x18, &results));
    assert_rnp_success(rnp_import_signatures(ffi, input, 0, NULL));
    assert_rnp_success(rnp_input_destroy(input));
    /* import signature again, making sure it is not duplicated */
    assert_true(
      check_import_sigs(ffi, &jso, &jsosigs, "data/test_key_validity/alice-rev.pgp"));
    assert_int_equal(json_object_array_length(jsosigs), 1);
    jsosig = json_object_array_get_idx(jsosigs, 0);
    assert_true(check_sig_status(
      jsosig, "unchanged", "unknown key", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    json_object_put(jso);
    /* check signature count, using the same key handle (it must not be changed) */
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 1);
    rnp_key_handle_destroy(key_handle);
    /* save and reload keyring, making sure signature is saved */
    reload_pubring(&ffi);
    /* find key and check sig count and revocation status */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_is_valid(key_handle, &valid));
    assert_false(valid);
    assert_rnp_success(rnp_key_valid_till(key_handle, &till));
    assert_int_equal(till, 1578663151);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));

    /* try to import wrong signature (certification) */
    assert_true(
      check_import_sigs(ffi, &jso, &jsosigs, "data/test_key_validity/alice-cert.pgp"));
    assert_int_equal(json_object_array_length(jsosigs), 1);
    jsosig = json_object_array_get_idx(jsosigs, 0);
    assert_true(check_sig_status(jsosig, "none", "none", NULL));
    json_object_put(jso);

    /* try to import signature for both public and secret key */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-pub.asc"));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sec.asc"));
    assert_true(
      check_import_sigs(ffi, &jso, &jsosigs, "data/test_key_validity/alice-rev.pgp"));
    assert_int_equal(json_object_array_length(jsosigs), 1);
    jsosig = json_object_array_get_idx(jsosigs, 0);
    assert_true(
      check_sig_status(jsosig, "new", "new", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    json_object_put(jso);

    /* import direct-key signature (with revocation key subpacket) */
    assert_true(
      check_import_sigs(ffi, &jso, &jsosigs, "data/test_key_validity/alice-revoker-sig.pgp"));
    assert_int_equal(json_object_array_length(jsosigs), 1);
    jsosig = json_object_array_get_idx(jsosigs, 0);
    assert_true(
      check_sig_status(jsosig, "new", "new", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    json_object_put(jso);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 2);
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_get_signature_at(key_handle, 1, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &type));
    assert_string_equal(type, "direct");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));

    /* load two binary signatures from the file */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-pub.asc"));

    assert_true(
      check_import_sigs(ffi, &jso, &jsosigs, "data/test_key_validity/alice-sigs.pgp"));
    assert_int_equal(json_object_array_length(jsosigs), 2);
    jsosig = json_object_array_get_idx(jsosigs, 0);
    assert_true(check_sig_status(
      jsosig, "new", "unknown key", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    jsosig = json_object_array_get_idx(jsosigs, 1);
    assert_true(check_sig_status(
      jsosig, "new", "unknown key", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    json_object_put(jso);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 2);
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));

    /* load two armored signatures from the single file */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sec.asc"));

    assert_true(
      check_import_sigs(ffi, &jso, &jsosigs, "data/test_key_validity/alice-sigs.asc"));
    assert_int_equal(json_object_array_length(jsosigs), 2);
    jsosig = json_object_array_get_idx(jsosigs, 0);
    /* when secret key is loaded then public copy is created automatically */
    assert_true(
      check_sig_status(jsosig, "new", "new", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    jsosig = json_object_array_get_idx(jsosigs, 1);
    assert_true(
      check_sig_status(jsosig, "new", "new", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    json_object_put(jso);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 2);
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    /* try to import signature from key file - must fail */
    assert_rnp_success(rnp_input_from_path(&input, "data/test_key_validity/alice-sec.asc"));
    results = NULL;
    assert_rnp_failure(rnp_import_signatures(ffi, input, 0, &results));
    assert_null(results);
    assert_rnp_success(rnp_input_destroy(input));
    /* try to import signatures from stream where second is malformed. Nothing should be
     * imported. */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-pub.asc"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_validity/alice-sigs-malf.pgp"));
    results = NULL;
    assert_rnp_failure(rnp_import_signatures(ffi, input, 0, &results));
    assert_null(results);
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 0);
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_false(revoked);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_export_revocation)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sec.asc"));

    rnp_key_handle_t key_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_null(&output));
    /* check for failure with wrong parameters */
    assert_rnp_failure(rnp_key_export_revocation(
      NULL, output, 0, "SHA256", "superseded", "test key revocation"));
    assert_rnp_failure(rnp_key_export_revocation(key_handle, NULL, 0, "SHA256", NULL, NULL));
    assert_rnp_failure(
      rnp_key_export_revocation(key_handle, output, 0x17, "SHA256", NULL, NULL));
    assert_rnp_failure(
      rnp_key_export_revocation(key_handle, output, 0, "Wrong hash", NULL, NULL));
    assert_rnp_failure(
      rnp_key_export_revocation(key_handle, output, 0, "SHA256", "Wrong reason code", NULL));
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    /* check for failure with subkey */
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &key_handle));
    assert_rnp_success(rnp_key_unlock(key_handle, "password"));
    assert_rnp_failure(rnp_key_export_revocation(
      key_handle, output, 0, "SHA256", "superseded", "test key revocation"));
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    /* try to export revocation having public key only */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_failure(rnp_key_export_revocation(
      key_handle, output, 0, "SHA256", "superseded", "test key revocation"));
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    /* load secret key and export revocation - should succeed with correct password */
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sec.asc"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    /* wrong password - must fail */
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "wrong"));
    assert_rnp_failure(rnp_key_export_revocation(
      key_handle, output, 0, "SHA256", "superseded", "test key revocation"));
    assert_rnp_failure(rnp_key_export_revocation(key_handle,
                                                 output,
                                                 RNP_KEY_EXPORT_ARMORED,
                                                 "SHA256",
                                                 "superseded",
                                                 "test key revocation"));

    /* unlocked key - must succeed */
    assert_rnp_success(rnp_key_unlock(key_handle, "password"));
    assert_rnp_success(rnp_key_export_revocation(key_handle, output, 0, "SHA256", NULL, NULL));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_output_to_path(&output, "alice-revocation.pgp"));
    /* correct password provider - must succeed */
    assert_rnp_success(rnp_key_lock(key_handle));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password"));
    assert_rnp_success(rnp_key_export_revocation(
      key_handle, output, 0, "SHA256", "superseded", "test key revocation"));
    assert_rnp_success(rnp_output_destroy(output));

    /* check that the output is binary or armored as requested */
    std::string data = file_to_str("alice-revocation.pgp");
    assert_false(starts_with(data, "-----BEGIN PGP PUBLIC KEY BLOCK-----"));
    assert_false(ends_with(strip_eol(data), "-----END PGP PUBLIC KEY BLOCK-----"));

    /* make sure FFI locks key back */
    bool locked = false;
    assert_rnp_success(rnp_key_is_locked(key_handle, &locked));
    assert_true(locked);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));
    /* make sure we can successfully import exported revocation */
    json_object *jso = NULL;
    json_object *jsosigs = NULL;
    assert_true(check_import_sigs(ffi, &jso, &jsosigs, "alice-revocation.pgp"));
    assert_int_equal(json_object_array_length(jsosigs), 1);
    json_object *jsosig = json_object_array_get_idx(jsosigs, 0);
    assert_true(
      check_sig_status(jsosig, "new", "new", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    json_object_put(jso);
    /* key now must become revoked */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    bool revoked = false;
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    /* check signature number - it now must be 1 */
    size_t sigcount = 0;
    assert_rnp_success(rnp_key_get_signature_count(key_handle, &sigcount));
    assert_int_equal(sigcount, 1);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));

    /* check signature contents */
    pgp_source_t src = {};
    assert_rnp_success(init_file_src(&src, "alice-revocation.pgp"));
    pgp::pkt::Signature sig;
    assert_rnp_success(sig.parse(src));
    src.close();
    assert_int_equal(sig.type(), PGP_SIG_REV_KEY);
    assert_true(sig.has_subpkt(PGP_SIG_SUBPKT_REVOCATION_REASON));
    assert_true(sig.has_keyfp());
    assert_int_equal(sig.revocation_code(), PGP_REVOCATION_SUPERSEDED);
    assert_string_equal(sig.revocation_reason().c_str(), "test key revocation");

    assert_int_equal(rnp_unlink("alice-revocation.pgp"), 0);
    assert_rnp_success(rnp_ffi_destroy(ffi));

    /* testing armored revocation generation */

    // load initial keyring
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sec.asc"));

    key_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));

    // export revocation
    assert_rnp_success(rnp_output_to_path(&output, "alice-revocation.asc"));
    assert_rnp_success(rnp_key_unlock(key_handle, "password"));
    assert_rnp_success(rnp_key_export_revocation(key_handle,
                                                 output,
                                                 RNP_KEY_EXPORT_ARMORED,
                                                 "SHA256",
                                                 "superseded",
                                                 "test key revocation"));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_key_handle_destroy(key_handle));

    data = file_to_str("alice-revocation.asc");
    assert_true(starts_with(data, "-----BEGIN PGP PUBLIC KEY BLOCK-----"));
    assert_true(ends_with(strip_eol(data), "-----END PGP PUBLIC KEY BLOCK-----"));

    // import it back
    assert_true(check_import_sigs(ffi, &jso, &jsosigs, "alice-revocation.asc"));
    assert_int_equal(json_object_array_length(jsosigs), 1);
    jsosig = json_object_array_get_idx(jsosigs, 0);
    assert_true(
      check_sig_status(jsosig, "new", "new", "73edcc9119afc8e2dbbdcde50451409669ffde3c"));
    json_object_put(jso);

    // make sure that key becomes revoked
    key_handle = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key_handle));
    assert_rnp_success(rnp_key_is_revoked(key_handle, &revoked));
    assert_true(revoked);
    assert_rnp_success(rnp_key_handle_destroy(key_handle));

    assert_int_equal(rnp_unlink("alice-revocation.asc"), 0);
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

#define KEYSIG_PATH "data/test_key_validity/"

TEST_F(rnp_tests, test_ffi_sig_validity)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* Case1:
     * Keys: Alice [pub]
     * Alice is signed by Basil, but without the Basil's key.
     * Result: Alice [valid]
     */
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "case1/pubring.gpg"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    bool valid = false;
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_signature_handle_t sig = NULL;
    /* signature 0: valid self-signature */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    char *sigtype = NULL;
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (positive)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    /* signature 1: valid certification from Basil, but without Basil's key */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (generic)");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_KEY_NOT_FOUND);
    size_t errors = 0;
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 1);
    uint32_t error = 0;
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_NO_SIGNER_KEY);
    /* let's load Basil's key and make sure signature is now validated and valid */
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "basil-pub.asc"));
    /* without revalidation previous check is cached */
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_KEY_NOT_FOUND);
    assert_rnp_success(rnp_signature_is_valid(sig, RNP_SIGNATURE_REVALIDATE));
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 0);
    rnp_signature_handle_destroy(sig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    /* Case2:
     * Keys: Alice [pub], Basil [pub]
     * Alice is signed by Basil, Basil is signed by Alice, but Alice's self-signature is
     * corrupted.
     * Result: Alice [invalid], Basil [valid]
     */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "case2/pubring.gpg"));
    /* Alice key */
    /* we cannot get key by uid since uid is invalid */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_null(key);
    /* get it via the fingerprint */
    assert_rnp_success(
      rnp_locate_key(ffi, "fingerprint", "73EDCC9119AFC8E2DBBDCDE50451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    /* signature 0: corrupted self-signature */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (positive)");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 2);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_LBITS_MISMATCH);
    assert_rnp_success(rnp_signature_error_at(sig, 1, &error));
    assert_int_equal(error, RNP_ERROR_SIGNATURE_INVALID);
    rnp_signature_handle_destroy(sig);
    /* signature 1: valid certification from Basil */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (generic)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 0);
    rnp_signature_handle_destroy(sig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    /* Basil key */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Basil <basil@rnp>", &key));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    /* signature 0: valid self-signature */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (positive)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    /* signature 1: valid certification from Alice */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (generic)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    /* Case3:
     * Keys: Alice [pub], Basil [pub]
     * Alice is signed by Basil, but doesn't have self-signature
     * Result: Alice [invalid]
     */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "case3/pubring.gpg"));
    /* Alice key */
    /* cannot locate it via userid since it is invalid */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_null(key);
    /* let's locate it via the fingerprint */
    assert_rnp_success(
      rnp_locate_key(ffi, "fingerprint", "73EDCC9119AFC8E2DBBDCDE50451409669FFDE3C", &key));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    /* signature 0: valid certification from Basil */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (generic)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    /* Case4:
     * Keys Alice [pub, sub]
     * Alice subkey has invalid binding signature
     * Result: Alice [valid], Alice sub [invalid]
     */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "case4/pubring.gpg"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    rnp_key_handle_t sub = NULL;
    rnp_key_get_subkey_at(key, 0, &sub);
    rnp_key_get_signature_at(sub, 0, &sig);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 1);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIGNATURE_INVALID);
    rnp_signature_handle_destroy(sig);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* Case5:
     * Keys Alice [pub, sub], Basil [pub]
     * Alice subkey has valid binding signature, but from the key Basil
     * Result: Alice [valid], Alice sub [invalid]
     */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "case5/pubring.gpg"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    rnp_key_get_subkey_at(key, 0, &sub);
    rnp_key_get_signature_at(sub, 0, &sig);
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "subkey binding");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 1);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_WRONG_BINDING);
    assert_rnp_failure(rnp_signature_error_at(sig, 1, &error));
    rnp_signature_handle_destroy(sig);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* Case6:
     * Keys Alice [pub, sub]
     * Key Alice has revocation signature by Alice, and subkey doesn't
     * Result: Alice [invalid], Alice sub [invalid]
     */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "case6/pubring.gpg"));
    /* check revocation signature */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    rnp_key_get_signature_at(key, 0, &sig);
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "key revocation");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    /* check subkey binding */
    rnp_key_get_subkey_at(key, 0, &sub);
    rnp_key_get_signature_at(sub, 0, &sig);
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "subkey binding");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* Case7:
     * Keys Alice [pub, sub]
     * Alice subkey has revocation signature by Alice
     * Result: Alice [valid], Alice sub [invalid]
     */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "case7/pubring.gpg"));
    /* check subkey revocation signature */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    rnp_key_get_subkey_at(key, 0, &sub);
    rnp_key_get_signature_at(sub, 0, &sig);
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "subkey revocation");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    /* check subkey binding */
    rnp_key_get_signature_at(sub, 1, &sig);
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "subkey binding");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    rnp_key_handle_destroy(sub);
    rnp_key_handle_destroy(key);

    /* Case8:
     * Keys Alice [pub, sub]
     * Userid is stripped from the key, but it still has valid subkey binding
     * Result: Alice [valid], Alice sub[valid]
     */

    /* not interesting for us at the moment */

    /* Case9:
     * Keys Alice [pub, sub]
     * Alice key has two self-signatures, one which expires key and second without key
     * expiration.
     * Result: Alice [valid], Alice sub[valid]
     */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "case9/pubring.gpg"));
    /* Alice key */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    /* signature 0: valid certification */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (positive)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    /* signature 1: valid certification */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (positive)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    /* another case: expired certification */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "alice-expired-claus-cert.asc"));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "claus-pub.asc"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    /* signature 0: valid certification */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (positive)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    /* signature 1: expired claus's certification */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (generic)");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_EXPIRED);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 1);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_EXPIRED);
    uint32_t expires = 0;
    assert_rnp_success(rnp_signature_get_expiration(sig, &expires));
    assert_int_equal(expires, 86400);
    uint32_t features = 0;
    assert_rnp_failure(rnp_signature_get_features(NULL, &features));
    assert_rnp_failure(rnp_signature_get_features(sig, NULL));
    assert_rnp_success(rnp_signature_get_features(sig, &features));
    assert_int_equal(features, 0);
    rnp_signature_handle_destroy(sig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_get_signature_type)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(load_keys_gpg(ffi, "data/test_key_edge_cases/alice-sig-misc-values.pgp"));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "basil-pub.asc"));

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    rnp_signature_handle_t sig = NULL;
    /* signature 0: valid self-signature */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    char *sigtype = NULL;
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (positive)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    /* signature 1: valid signature by Basil */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (generic)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_is_valid(sig, 0));
    rnp_signature_handle_destroy(sig);
    /* signature 2..7: invalid signatures with misc types */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 2, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "standalone");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    size_t errors = 0;
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 1);
    rnp_result_t error = 0;
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_WRONG_KEY_SIG);
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 3, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (persona)");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 2);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_LBITS_MISMATCH);
    assert_rnp_success(rnp_signature_error_at(sig, 1, &error));
    assert_int_equal(error, RNP_ERROR_SIGNATURE_INVALID);
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 4, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (casual)");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 2);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_LBITS_MISMATCH);
    assert_rnp_success(rnp_signature_error_at(sig, 1, &error));
    assert_int_equal(error, RNP_ERROR_SIGNATURE_INVALID);
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 5, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "primary key binding");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 1);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_WRONG_KEY_SIG);
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 6, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification revocation");
    rnp_buffer_destroy(sigtype);
    assert_int_equal(rnp_signature_is_valid(sig, 0), RNP_ERROR_SIGNATURE_INVALID);
    assert_rnp_success(rnp_signature_error_count(sig, &errors));
    assert_int_equal(errors, 2);
    assert_rnp_success(rnp_signature_error_at(sig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_LBITS_MISMATCH);
    assert_rnp_success(rnp_signature_error_at(sig, 1, &error));
    assert_int_equal(error, RNP_ERROR_SIGNATURE_INVALID);
    rnp_signature_handle_destroy(sig);

    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);

    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_remove_signature)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(load_keys_gpg(ffi, "data/test_key_edge_cases/alice-sig-misc-values.pgp"));
    assert_true(import_pub_keys(ffi, KEYSIG_PATH "basil-pub.asc"));

    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    size_t count = 255;
    assert_rnp_success(rnp_key_get_signature_count(key, &count));
    assert_int_equal(count, 0);
    rnp_key_handle_t bkey = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Basil <basil@rnp>", &bkey));
    count = 255;
    assert_rnp_success(rnp_key_get_signature_count(bkey, &count));
    assert_int_equal(count, 0);

    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    rnp_signature_handle_t sig = NULL;
    assert_rnp_success(rnp_uid_get_signature_count(uid, &count));
    assert_int_equal(count, 8);
    /* signature 1: valid signature by Basil */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &sig));
    assert_rnp_failure(rnp_signature_remove(NULL, sig));
    assert_rnp_failure(rnp_signature_remove(key, NULL));
    /* attempt to delete signature from the wrong key */
    assert_int_equal(rnp_signature_remove(bkey, sig), RNP_ERROR_NO_SIGNATURES_FOUND);
    assert_rnp_success(rnp_signature_remove(key, sig));
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_uid_get_signature_count(uid, &count));
    assert_int_equal(count, 7);
    /* signature 2: must be moved to position 1 */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &sig));
    char *sigtype = NULL;
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "standalone");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_remove(key, sig));
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_uid_get_signature_count(uid, &count));
    assert_int_equal(count, 6);
    /* signature 7: must be moved to position 5 */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 5, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "third-party");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_remove(key, sig));
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_uid_get_signature_count(uid, &count));
    assert_int_equal(count, 5);
    /* check that key and userid are still valid */
    bool valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    valid = false;
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(bkey);

    /* Export key and reload */
    reload_pubring(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    count = 255;
    assert_rnp_success(rnp_key_get_signature_count(key, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Basil <basil@rnp>", &bkey));
    count = 255;
    assert_rnp_success(rnp_key_get_signature_count(bkey, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_get_signature_count(uid, &count));
    assert_int_equal(count, 5);
    /* delete self-certification and make sure that key/uid become invalid */
    valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    valid = false;
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "certification (positive)");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_remove(key, sig));
    rnp_signature_handle_destroy(sig);
    valid = true;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_false(valid);
    valid = true;
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_false(valid);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(bkey);

    /* Remove subkey's signature */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(load_keys_gpg(ffi, "data/test_key_validity/case7/pubring.gpg"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    count = 0;
    assert_rnp_success(rnp_key_get_subkey_count(key, &count));
    assert_int_equal(count, 1);
    rnp_key_handle_t sub = NULL;
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    count = 255;
    assert_rnp_success(rnp_key_get_signature_count(sub, &count));
    assert_int_equal(count, 2);
    /* check whether key and sub valid: [true, false] since sub is revoked */
    valid = false;
    assert_rnp_success(rnp_key_is_valid(key, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_false(valid);
    assert_rnp_success(rnp_key_get_signature_at(sub, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "subkey revocation");
    rnp_buffer_destroy(sigtype);
    assert_rnp_success(rnp_signature_remove(sub, sig));
    rnp_signature_handle_destroy(sig);
    /* now subkey must become valid with 1 signature */
    assert_rnp_success(rnp_key_get_signature_count(sub, &count));
    assert_int_equal(count, 1);
    valid = false;
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);
    /* reload keys */
    reload_pubring(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    valid = false;
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_true(valid);
    assert_rnp_success(rnp_key_get_signature_at(sub, 0, &sig));
    assert_rnp_success(rnp_signature_get_type(sig, &sigtype));
    assert_string_equal(sigtype, "subkey binding");
    rnp_buffer_destroy(sigtype);
    assert_rnp_failure(rnp_signature_remove(key, sig));
    assert_rnp_success(rnp_signature_remove(sub, sig));
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_key_is_valid(sub, &valid));
    assert_false(valid);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_failure(rnp_signature_remove(sub, sig));
    assert_rnp_success(rnp_signature_remove(key, sig));
    rnp_signature_handle_destroy(sig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);
    /* save and reload keys without sigs */
    reload_pubring(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    count = 255;
    assert_rnp_success(rnp_key_get_signature_count(key, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    count = 255;
    assert_rnp_success(rnp_uid_get_signature_count(uid, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    count = 255;
    assert_rnp_success(rnp_key_get_signature_count(sub, &count));
    assert_int_equal(count, 0);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);

    /* Remove signature from the secret key/subkey */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(load_keys_gpg(ffi,
                              "data/test_key_validity/alice-sub-pub.pgp",
                              "data/test_key_validity/alice-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "Alice <alice@rnp>", &key));
    /* make sure they are actually secret */
    bool secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    assert_rnp_success(rnp_key_have_secret(sub, &secret));
    assert_true(secret);
    /* remove both signatures and reload */
    assert_rnp_success(rnp_key_get_signature_at(sub, 0, &sig));
    assert_rnp_success(rnp_signature_remove(sub, sig));
    rnp_signature_handle_destroy(sig);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &sig));
    assert_rnp_success(rnp_signature_remove(key, sig));
    rnp_signature_handle_destroy(sig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);
    reload_keyrings(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(key, &secret));
    assert_true(secret);
    count = 255;
    assert_rnp_success(rnp_key_get_signature_count(key, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    count = 255;
    assert_rnp_success(rnp_uid_get_signature_count(uid, &count));
    assert_int_equal(count, 0);
    assert_rnp_success(rnp_key_get_subkey_at(key, 0, &sub));
    secret = false;
    assert_rnp_success(rnp_key_have_secret(sub, &secret));
    assert_true(secret);
    count = 255;
    assert_rnp_success(rnp_key_get_signature_count(sub, &count));
    assert_int_equal(count, 0);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(sub);

    assert_rnp_success(rnp_ffi_destroy(ffi));
}

static std::string
key_info(rnp_key_handle_t key)
{
    bool sec = false;
    rnp_key_have_secret(key, &sec);
    bool primary = false;
    rnp_key_is_primary(key, &primary);
    std::string res = ":";
    res += primary ? (sec ? "sec" : "pub") : (sec ? "ssb" : "sub");
    char *keyid = NULL;
    rnp_key_get_keyid(key, &keyid);
    res += std::string("(") + std::string(keyid, 4) + std::string(")");
    rnp_buffer_destroy(keyid);
    return res;
}

static std::string
sig_info(rnp_signature_handle_t sig)
{
    int      type = sig->sig->sig.type();
    char *   keyid = NULL;
    uint32_t sigid = sig->sig->sigid[0] + (sig->sig->sigid[1] << 8);
    rnp_signature_get_keyid(sig, &keyid);
    std::stringstream ss;
    ss << ":sig(" << type << ", " << std::hex << sigid << ", " << std::string(keyid, 4) << ")";
    rnp_buffer_destroy(keyid);
    return ss.str();
}

static std::string
uid_info(rnp_uid_handle_t uid)
{
    std::string res;
    uint32_t    type = 0;
    rnp_uid_get_type(uid, &type);
    if (type == RNP_USER_ATTR) {
        res = ":uid(photo)";
    } else {
        char * uidstr = NULL;
        size_t len = 0;
        rnp_uid_get_data(uid, (void **) &uidstr, &len);
        res = ":uid(" + std::string(uidstr, uidstr + len) + ")";
        rnp_buffer_destroy(uidstr);
    }

    size_t sigs = 0;
    rnp_uid_get_signature_count(uid, &sigs);
    for (size_t i = 0; i < sigs; i++) {
        rnp_signature_handle_t sig = NULL;
        rnp_uid_get_signature_at(uid, i, &sig);
        res += sig_info(sig);
        rnp_signature_handle_destroy(sig);
    }
    return res;
}

static std::string
key_packets(rnp_key_handle_t key)
{
    std::string res = key_info(key);
    size_t      sigs = 0;
    rnp_key_get_signature_count(key, &sigs);
    for (size_t i = 0; i < sigs; i++) {
        rnp_signature_handle_t sig = NULL;
        rnp_key_get_signature_at(key, i, &sig);
        res += sig_info(sig);
        rnp_signature_handle_destroy(sig);
    }

    bool primary = false;
    rnp_key_is_primary(key, &primary);
    if (!primary) {
        return res;
    }

    size_t uids = 0;
    rnp_key_get_uid_count(key, &uids);
    for (size_t i = 0; i < uids; i++) {
        rnp_uid_handle_t uid = NULL;
        rnp_key_get_uid_handle_at(key, i, &uid);
        res += uid_info(uid);
        rnp_uid_handle_destroy(uid);
    }

    size_t subs = 0;
    rnp_key_get_subkey_count(key, &subs);
    for (size_t i = 0; i < subs; i++) {
        rnp_key_handle_t sub = NULL;
        rnp_key_get_subkey_at(key, i, &sub);
        res += key_packets(sub);
        rnp_key_handle_destroy(sub);
    }
    return res;
}

static void
sigremove_leave(rnp_ffi_t ffi, void *app_ctx, rnp_signature_handle_t sig, uint32_t *action)
{
    assert_true((*(int *) app_ctx) == 48);
    assert_non_null(sig);
    assert_non_null(action);
    assert_non_null(ffi);
    *action = RNP_KEY_SIGNATURE_KEEP;
}

static void
sigremove_unchanged(rnp_ffi_t ffi, void *app_ctx, rnp_signature_handle_t sig, uint32_t *action)
{
    assert_true((*(int *) app_ctx) == 48);
    assert_non_null(sig);
    assert_non_null(action);
    assert_non_null(ffi);
}

static void
sigremove_remove(rnp_ffi_t ffi, void *app_ctx, rnp_signature_handle_t sig, uint32_t *action)
{
    assert_true((*(int *) app_ctx) == 48);
    *action = RNP_KEY_SIGNATURE_REMOVE;
}

static void
sigremove_revocation(rnp_ffi_t              ffi,
                     void *                 app_ctx,
                     rnp_signature_handle_t sig,
                     uint32_t *             action)
{
    assert_true((*(int *) app_ctx) == 48);
    char *type = NULL;
    assert_rnp_success(rnp_signature_get_type(sig, &type));
    if (std::string(type).find("revocation") != std::string::npos) {
        *action = RNP_KEY_SIGNATURE_REMOVE;
    } else {
        *action = RNP_KEY_SIGNATURE_KEEP;
    }
    rnp_buffer_destroy(type);
}

TEST_F(rnp_tests, test_ffi_remove_signatures)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* case 1: key Alice with self-signature and certification from the Basil. */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case1/pubring.gpg"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sig(16, de9d, 0B2B)");
    /* rnp_key_remove_signatures corner cases */
    assert_rnp_failure(rnp_key_remove_signatures(NULL, RNP_KEY_SIGNATURE_INVALID, NULL, NULL));
    assert_rnp_failure(rnp_key_remove_signatures(NULL, 0, NULL, NULL));
    assert_rnp_failure(rnp_key_remove_signatures(key, 0, NULL, ffi));
    assert_rnp_failure(rnp_key_remove_signatures(key, 0x77, NULL, ffi));
    /* remove unknown signatures */
    assert_rnp_success(
      rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_UNKNOWN_KEY, NULL, NULL));
    /* signature is deleted since we don't have Basil's key in the keyring */
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451)");
    /* let's load key and try again */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case1/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/basil-pub.asc"));
    assert_rnp_success(
      rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_UNKNOWN_KEY, NULL, NULL));
    /* now it is not removed */
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sig(16, de9d, 0B2B)");
    /* let's delete non-self sigs */
    assert_rnp_success(
      rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_NON_SELF_SIG, NULL, NULL));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451)");
    rnp_key_handle_destroy(key);
    /* case 2: alice with corrupted self-signature */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case2/pubring.gpg"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669ffde3c", &key));
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0451):uid(Alice <alice@rnp>):sig(19, e530, 0451):sig(16, 2508, 0B2B)");
    /* remove invalid signature */
    assert_rnp_success(rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_INVALID, NULL, NULL));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(16, 2508, 0B2B)");
    /* remove both invalid and non-self signatures */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case2/pubring.gpg"));
    assert_rnp_success(rnp_key_remove_signatures(
      key, RNP_KEY_SIGNATURE_INVALID | RNP_KEY_SIGNATURE_NON_SELF_SIG, NULL, NULL));
    assert_string_equal(key_packets(key).c_str(), ":pub(0451):uid(Alice <alice@rnp>)");

    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));
    /* load both keyrings and remove Basil's key */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case1/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case2/pubring.gpg"));

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0B2B09F7D7EA6E0E", &key));
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0B2B):uid(Basil <basil@rnp>):sig(19, f083, 0B2B):sig(16, a7cd, 0451)");

    assert_rnp_success(rnp_key_remove(key, RNP_KEY_REMOVE_PUBLIC | RNP_KEY_REMOVE_SUBKEYS));
    rnp_key_handle_destroy(key);

    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sig(16, de9d, "
                        "0B2B):sig(19, e530, 0451):sig(16, 2508, 0B2B)");
    assert_rnp_success(rnp_key_remove_signatures(
      key, RNP_KEY_SIGNATURE_INVALID | RNP_KEY_SIGNATURE_UNKNOWN_KEY, NULL, NULL));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451)");
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC));

    /* case 4: alice key with invalid subkey bindings (corrupted and non-self) */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case4/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case5/pubring.gpg"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, "
                        "0451):sub(DD23):sig(24, 89c0, 0451):sig(24, 1f6d, 0B2B)");
    assert_rnp_success(rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_INVALID, NULL, NULL));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sub(DD23)");
    /* make sure non-self doesn't touch invalid self sigs */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case4/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case5/pubring.gpg"));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, "
                        "0451):sub(DD23):sig(24, 89c0, 0451):sig(24, 1f6d, 0B2B)");
    assert_rnp_success(
      rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_NON_SELF_SIG, NULL, NULL));
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sub(DD23):sig(24, 89c0, 0451)");
    /* add subkey with valid subkey binding */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case4/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case5/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/pubring.gpg"));
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sub(DD23):sig(24, 89c0, "
      "0451):sig(24, 1f6d, 0B2B):sub(22F3):sig(24, 3766, 0451)");
    assert_rnp_success(rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_INVALID, NULL, NULL));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, "
                        "0451):sub(DD23):sub(22F3):sig(24, 3766, 0451)");

    /* load more keys and signatures and check callback usage */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case1/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case2/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case5/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case6/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case7/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case9/pubring.gpg"));
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0451):sig(32, c76f, 0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sig(16, "
      "de9d, 0B2B):sig(19, e530, 0451):sig(16, 2508, 0B2B):sig(19, b22f, 0451):sig(19, 6cd1, "
      "0451):sub(DD23):sig(24, 1f6d, 0B2B):sig(24, ea55, 0451):sig(40, f001, "
      "0451):sub(22F3):sig(24, 3766, 0451)");
    int param = 48;
    assert_rnp_success(rnp_key_remove_signatures(key,
                                                 RNP_KEY_SIGNATURE_INVALID |
                                                   RNP_KEY_SIGNATURE_UNKNOWN_KEY |
                                                   RNP_KEY_SIGNATURE_NON_SELF_SIG,
                                                 sigremove_leave,
                                                 &param));
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0451):sig(32, c76f, 0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sig(16, "
      "de9d, 0B2B):sig(19, e530, 0451):sig(16, 2508, 0B2B):sig(19, b22f, 0451):sig(19, 6cd1, "
      "0451):sub(DD23):sig(24, 1f6d, 0B2B):sig(24, ea55, 0451):sig(40, f001, "
      "0451):sub(22F3):sig(24, 3766, 0451)");

    assert_rnp_success(
      rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_INVALID, sigremove_unchanged, &param));
    assert_string_equal(
      key_packets(key).c_str(),
      ":pub(0451):sig(32, c76f, 0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sig(16, "
      "de9d, 0B2B):sig(16, 2508, 0B2B):sig(19, b22f, 0451):sig(19, 6cd1, "
      "0451):sub(DD23):sig(24, ea55, 0451):sig(40, f001, 0451):sub(22F3):sig(24, 3766, 0451)");

    assert_rnp_success(rnp_key_remove_signatures(
      key, RNP_KEY_SIGNATURE_NON_SELF_SIG, sigremove_revocation, &param));
    assert_string_equal(key_packets(key).c_str(),
                        ":pub(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sig(16, de9d, "
                        "0B2B):sig(16, 2508, 0B2B):sig(19, b22f, 0451):sig(19, 6cd1, "
                        "0451):sub(DD23):sig(24, ea55, 0451):sub(22F3):sig(24, 3766, 0451)");

    /* make sure that signature will be removed from the secret key as well */
    rnp_key_handle_destroy(key);
    assert_true(import_sec_keys(ffi, "data/test_key_validity/alice-sign-sub-sec.pgp"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_string_equal(key_packets(key).c_str(),
                        ":sec(0451):uid(Alice <alice@rnp>):sig(19, 8ba5, 0451):sig(16, de9d, "
                        "0B2B):sig(16, 2508, 0B2B):sig(19, b22f, 0451):sig(19, 6cd1, "
                        "0451):sub(DD23):sig(24, ea55, 0451):ssb(22F3):sig(24, 3766, 0451)");
    assert_rnp_success(
      rnp_key_remove_signatures(key, RNP_KEY_SIGNATURE_INVALID, sigremove_remove, &param));
    assert_string_equal(key_packets(key).c_str(),
                        ":sec(0451):uid(Alice <alice@rnp>):sub(DD23):ssb(22F3)");
    rnp_key_handle_destroy(key);

    /* reload keyring, making sure changes are saved */
    reload_keyrings(&ffi);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "0451409669FFDE3C", &key));
    assert_string_equal(key_packets(key).c_str(),
                        ":sec(0451):uid(Alice <alice@rnp>):sub(DD23):ssb(22F3)");
    rnp_key_handle_destroy(key);
    /* load data and delete signatures on subkey */
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case6/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case7/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case8/pubring.gpg"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/case9/pubring.gpg"));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "DD23CEB7FEBEFF17", &key));
    assert_string_equal(key_packets(key).c_str(),
                        ":sub(DD23):sig(24, ea55, 0451):sig(40, f001, 0451)");
    assert_rnp_success(rnp_key_remove_signatures(key, 0, sigremove_revocation, &param));
    assert_string_equal(key_packets(key).c_str(), ":sub(DD23):sig(24, ea55, 0451)");
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_rsa_small_sig)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/rsa_key_small_sig-pub.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "ED23B0105947F283", &key));
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    bool valid = false;
    assert_rnp_success(rnp_uid_is_valid(uid, &valid));
    assert_true(valid);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_ffi_key_critical_notations)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* Load key with 2 unknown critical notations in certification */
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/key-critical-notations.pgp"));
    rnp_key_handle_t key = NULL;
    /* key is valid since it has valid subkey binding, but userid is not valid */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "critical-key", &key));
    assert_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "ddc610bb7b8f689c", &key));
    assert_non_null(key);
    assert_true(check_key_valid(key, true));
    /* uid is not valid, as certification has unknown critical notation */
    assert_true(check_uid_valid(key, 0, false));
    assert_true(check_sub_valid(key, 0, true));
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    /* Load key with unknown critical notations in both certification and binding */
    assert_true(import_all_keys(ffi, "data/test_key_edge_cases/key-sub-crit-note-pub.pgp"));
    /* key is not valid, as well as sub and uid */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "critical_notation", &key));
    assert_null(key);
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "9988c1bcb55391d6", &key));
    assert_non_null(key);
    assert_true(check_key_valid(key, false));
    assert_true(check_uid_valid(key, 0, false));
    assert_true(check_sub_valid(key, 0, false));
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);

    /* Verify data signature with unknown critical notation */
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(load_keys_gpg(ffi, "data/keyrings/1/pubring.gpg"));
    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_messages/message.txt.signed.crit-notation"));
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    size_t sigcount = 255;
    assert_rnp_success(rnp_op_verify_get_signature_count(verify, &sigcount));
    assert_int_equal(sigcount, 1);
    rnp_op_verify_signature_t sig = NULL;
    assert_rnp_success(rnp_op_verify_get_signature_at(verify, 0, &sig));
    assert_int_equal(rnp_op_verify_signature_get_status(sig), RNP_ERROR_SIGNATURE_INVALID);
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_import_invalid_issuer)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* public key + secret subkey with invalid signer's keyfp */
    rnp_input_t input = NULL;
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-sub-sig-fp.pgp"));
    char *   keys = NULL;
    uint32_t flags =
      RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_SINGLE;
    assert_rnp_success(rnp_import_keys(ffi, input, flags, &keys));
    rnp_input_destroy(input);
    rnp_buffer_destroy(keys);

    /* public key + secret subkey with invalid signer's keyid */
    assert_rnp_success(rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_key_edge_cases/alice-sub-sig-keyid.pgp"));
    assert_rnp_success(rnp_import_keys(ffi, input, flags, &keys));
    rnp_input_destroy(input);
    rnp_buffer_destroy(keys);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_add_revoker_signature)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-25519-2subs-sec.asc"));
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/ecc-p256-pub.asc"));
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/ecc-p384-pub.asc"));
    rnp_key_handle_t key = NULL;
    /* Locate key and make sure it doesn't have designated revokers */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-25519", &key));
    size_t count = 10;
    assert_rnp_success(rnp_key_get_revoker_count(key, &count));
    assert_int_equal(count, 0);
    /* Add designated revoker */
    rnp_key_handle_t revoker = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &revoker));
    rnp_signature_handle_t newsig = NULL;
    /* Create signature, including edge cases checks */
    assert_rnp_failure(rnp_key_direct_signature_create(NULL, NULL, &newsig));
    assert_rnp_failure(rnp_key_direct_signature_create(key, key, NULL));
    assert_rnp_failure(rnp_key_direct_signature_create(revoker, key, &newsig));
    assert_rnp_success(rnp_key_direct_signature_create(key, NULL, &newsig));
    /* Set revoker, including edge cases */
    assert_rnp_failure(rnp_key_signature_set_revoker(NULL, revoker, 0));
    assert_rnp_failure(rnp_key_signature_set_revoker(newsig, NULL, 0));
    assert_rnp_failure(rnp_key_signature_set_revoker(newsig, revoker, 0x33));
    assert_rnp_success(rnp_key_signature_set_revoker(newsig, revoker, 0));
    assert_rnp_success(rnp_key_signature_set_revoker(newsig, revoker, RNP_REVOKER_SENSITIVE));
    /* Attempt to validate non-finished signature */
    assert_rnp_failure(rnp_signature_is_valid(newsig, 0));
    /* Populate signature */
    assert_rnp_failure(rnp_key_signature_sign(NULL));
    assert_int_equal(rnp_key_signature_sign(newsig), RNP_ERROR_BAD_PASSWORD);
    rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "wrong1");
    assert_int_equal(rnp_key_signature_sign(newsig), RNP_ERROR_BAD_PASSWORD);
    rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password");
    assert_rnp_success(rnp_key_signature_sign(newsig));
    /* Check signature and key properties */
    char *revfp = NULL;
    assert_rnp_success(rnp_signature_get_revoker(newsig, &revfp));
    assert_string_equal(revfp, "B54FDEBBB673423A5D0AA54423674F21B2441527");
    rnp_buffer_destroy(revfp);
    assert_rnp_success(rnp_key_get_revoker_count(key, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_key_get_revoker_at(key, 0, &revfp));
    assert_string_equal(revfp, "B54FDEBBB673423A5D0AA54423674F21B2441527");
    rnp_buffer_destroy(revfp);
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    /* Attempt to sign already populated  signature */
    assert_rnp_failure(rnp_key_signature_set_revoker(newsig, revoker, 0));
    assert_rnp_failure(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    /* Make sure that newly added signature is first of the key's signatures */
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &newsig));
    assert_rnp_failure(rnp_key_signature_sign(newsig));
    char *type = NULL;
    assert_rnp_success(rnp_signature_get_type(newsig, &type));
    assert_string_equal(type, "direct");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_signature_get_revoker(newsig, &revfp));
    assert_string_equal(revfp, "B54FDEBBB673423A5D0AA54423674F21B2441527");
    rnp_buffer_destroy(revfp);
    rnp_signature_handle_destroy(newsig);
    /* Export key and make sure signature is exported */
    auto keydata = export_key(key, true);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(revoker);
    rnp_ffi_t newffi = NULL;
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    assert_rnp_success(rnp_locate_key(newffi, "userid", "ecc-25519", &key));
    assert_rnp_success(rnp_key_get_revoker_count(key, &count));
    assert_int_equal(count, 1);
    assert_rnp_success(rnp_key_get_revoker_at(key, 0, &revfp));
    assert_string_equal(revfp, "B54FDEBBB673423A5D0AA54423674F21B2441527");
    rnp_buffer_destroy(revfp);
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &newsig));
    assert_rnp_success(rnp_signature_get_type(newsig, &type));
    assert_string_equal(type, "direct");
    rnp_buffer_destroy(type);
    assert_rnp_success(rnp_signature_get_revoker(newsig, &revfp));
    assert_string_equal(revfp, "B54FDEBBB673423A5D0AA54423674F21B2441527");
    rnp_buffer_destroy(revfp);
    rnp_signature_handle_destroy(newsig);
    rnp_key_handle_destroy(key);
    /* Reload keyrings and make sure data is saved */
    assert_rnp_success(rnp_unload_keys(newffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET));
    rnp_ffi_destroy(newffi);
    /* Add second designated revoker and make sure it works */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-25519", &key));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p384", &revoker));
    assert_rnp_success(rnp_key_direct_signature_create(key, NULL, &newsig));
    assert_rnp_success(rnp_key_signature_set_revoker(newsig, revoker, 0));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    assert_rnp_success(rnp_signature_get_revoker(newsig, &revfp));
    assert_string_equal(revfp, "AB25CBA042DD924C3ACC3ED3242A3AA5EA85F44A");
    rnp_buffer_destroy(revfp);
    assert_rnp_success(rnp_key_get_revoker_count(key, &count));
    assert_int_equal(count, 2);
    assert_rnp_success(rnp_key_get_revoker_at(key, 0, &revfp));
    assert_string_equal(revfp, "AB25CBA042DD924C3ACC3ED3242A3AA5EA85F44A");
    rnp_buffer_destroy(revfp);
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    rnp_signature_handle_destroy(newsig);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(revoker);
    /* Attempt to add designatured revoker to subkey */
    rnp_key_handle_t subkey = NULL;
    assert_rnp_success(
      rnp_locate_key(ffi, "fingerprint", "6D207DCC0AC281DBFC285785573436C231AE6338", &subkey));
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p384", &revoker));
    assert_rnp_failure(rnp_key_direct_signature_create(subkey, NULL, &newsig));
    rnp_key_handle_destroy(revoker);
    /* Standard doesn't seem to answer whether subkey should be allowed here */
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-25519", &key));
    assert_rnp_success(rnp_key_direct_signature_create(key, NULL, &newsig));
    assert_rnp_success(rnp_key_signature_set_revoker(newsig, subkey, 0));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    rnp_key_handle_destroy(key);
    rnp_key_handle_destroy(subkey);
    /* Check v5 key */

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_create_revocation_signature)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-25519-2subs-sec.asc"));
    assert_true(import_pub_keys(ffi, "data/test_stream_key_load/ecc-p256-sec.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-25519", &key));
    /* Create self revocation */
    rnp_signature_handle_t newsig = NULL;
    assert_rnp_failure(rnp_key_revocation_signature_create(NULL, key, &newsig));
    assert_rnp_failure(rnp_key_revocation_signature_create(key, key, NULL));
    assert_rnp_failure(rnp_key_revocation_signature_create(key, NULL, NULL));
    assert_rnp_success(rnp_key_revocation_signature_create(key, NULL, &newsig));
    const char *revcode = "compromised";
    const char *revreason = "custom revocation reason";
    const char *hash = "SHA512";
    assert_rnp_failure(rnp_key_signature_set_hash(NULL, hash));
    assert_rnp_failure(rnp_key_signature_set_hash(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_set_hash(newsig, "wrong"));
    assert_rnp_success(rnp_key_signature_set_hash(newsig, hash));
    assert_rnp_failure(rnp_key_signature_set_revocation_reason(NULL, revcode, revreason));
    assert_rnp_success(rnp_key_signature_set_revocation_reason(newsig, NULL, NULL));
    assert_rnp_success(rnp_key_signature_set_revocation_reason(newsig, NULL, "wrong reason"));
    assert_rnp_failure(rnp_key_signature_set_revocation_reason(newsig, "wrong code", NULL));
    assert_rnp_success(rnp_key_signature_set_revocation_reason(newsig, revcode, revreason));
    assert_rnp_failure(rnp_key_signature_sign(newsig));
    rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password");
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    /* Check signature parameters */
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &newsig));
    assert_true(check_sig_hash(newsig, hash));
    assert_true(check_sig_type(newsig, "key revocation"));
    char *sigcode = NULL;
    char *sigreason = NULL;
    /* Some edge cases of the new function */
    assert_rnp_failure(rnp_signature_get_revocation_reason(NULL, &sigcode, &sigreason));
    assert_rnp_success(rnp_signature_get_revocation_reason(newsig, &sigcode, NULL));
    assert_string_equal(sigcode, revcode);
    rnp_buffer_destroy(sigcode);
    assert_rnp_success(rnp_signature_get_revocation_reason(newsig, NULL, &sigreason));
    assert_string_equal(sigreason, revreason);
    rnp_buffer_destroy(sigreason);
    assert_true(check_sig_revreason(newsig, revcode, revreason));
    rnp_signature_handle_destroy(newsig);
    assert_true(check_key_revoked(key, true));
    assert_true(check_key_revreason(key, revreason));
    bool compromised = false;
    assert_rnp_success(rnp_key_is_compromised(key, &compromised));
    assert_true(compromised);
    /* Export key and make sure data is saved */
    auto keydata = export_key(key, true);
    rnp_key_handle_destroy(key);
    rnp_ffi_t newffi = NULL;
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    assert_rnp_success(rnp_locate_key(newffi, "userid", "ecc-25519", &key));
    assert_true(check_key_revoked(key, true));
    assert_true(check_key_revreason(key, revreason));
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &newsig));
    assert_true(check_sig_type(newsig, "key revocation"));
    rnp_signature_handle_destroy(newsig);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(newffi);
    /* Create revocation for other key, using the designated revoker */
    rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET);
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-25519-sec.asc"));
    assert_true(
      import_pub_keys(ffi, "data/test_stream_key_load/ecc-p256-desig-rev-1-pub.asc"));
    key = get_key_by_uid(ffi, "ecc-p256");
    rnp_key_handle_t revoker = get_key_by_uid(ffi, "ecc-25519");
    assert_true(check_key_locked(revoker, true));
    assert_true(check_key_revoked(key, false));
    assert_rnp_success(rnp_key_revocation_signature_create(revoker, key, &newsig));
    assert_rnp_success(rnp_key_signature_set_hash(newsig, hash));
    assert_rnp_success(rnp_key_signature_set_revocation_reason(newsig, revcode, revreason));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    assert_true(check_key_locked(revoker, true));
    rnp_signature_handle_destroy(newsig);
    rnp_key_handle_destroy(revoker);
    assert_true(check_key_revoked(key, true));
    assert_true(check_key_revreason(key, revreason));
    assert_rnp_success(rnp_key_is_compromised(key, &compromised));
    assert_true(compromised);
    /* Export/import and check */
    keydata = export_key(key);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    assert_true(import_all_keys(newffi, "data/test_stream_key_load/ecc-25519-pub.asc"));
    key = get_key_by_uid(ffi, "ecc-p256");
    assert_true(check_key_revoked(key, true));
    assert_true(check_key_revreason(key, revreason));
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &newsig));
    assert_true(check_sig_type(newsig, "key revocation"));
    rnp_signature_handle_destroy(newsig);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(newffi);
    /* Create self subkey revocation */
    rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET);
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-p256-sec.asc"));
    key = get_key_by_uid(ffi, "ecc-p256");
    rnp_key_handle_t subkey = get_key_by_fp(ffi, "40E608AFBC8D62CDCC08904F37E285E9E9851491");
    assert_rnp_success(rnp_key_revocation_signature_create(key, subkey, &newsig));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    assert_true(check_key_revoked(subkey, true));
    assert_true(check_key_revreason(subkey, "No reason specified"));
    assert_rnp_success(rnp_key_is_compromised(subkey, &compromised));
    assert_false(compromised);
    rnp_signature_handle_destroy(newsig);
    assert_rnp_success(rnp_key_get_signature_at(subkey, 0, &newsig));
    assert_true(check_sig_type(newsig, "subkey revocation"));
    assert_rnp_success(rnp_signature_get_revocation_reason(newsig, &sigcode, &sigreason));
    assert_string_equal(sigcode, "");
    assert_string_equal(sigreason, "");
    rnp_buffer_destroy(sigcode);
    rnp_buffer_destroy(sigreason);
    rnp_signature_handle_destroy(newsig);
    rnp_key_handle_destroy(subkey);
    /* Export and recheck */
    keydata = export_key(key);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    subkey = get_key_by_fp(newffi, "40E608AFBC8D62CDCC08904F37E285E9E9851491");
    assert_true(check_key_revoked(subkey, true));
    assert_true(check_key_revreason(subkey, "No reason specified"));
    assert_rnp_success(rnp_key_is_compromised(subkey, &compromised));
    assert_false(compromised);
    rnp_key_handle_destroy(subkey);
    rnp_ffi_destroy(newffi);
    /* Revoke other key using subkey as revoker with designated revoker */
    rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET);
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-25519-2subs-sec.asc"));
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-p256-subkey-revoker.asc"));
    key = get_key_by_uid(ffi, "ecc-p256");
    revoker = get_key_by_fp(ffi, "6D207DCC0AC281DBFC285785573436C231AE6338");
    assert_rnp_success(rnp_key_revocation_signature_create(revoker, key, &newsig));
    assert_rnp_success(rnp_key_signature_set_hash(newsig, hash));
    assert_rnp_success(rnp_key_signature_set_revocation_reason(newsig, revcode, revreason));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    assert_true(check_key_revoked(key, true));
    assert_true(check_key_revreason(key, revreason));
    assert_rnp_success(rnp_key_is_compromised(key, &compromised));
    assert_true(compromised);
    rnp_key_handle_destroy(revoker);
    /* Export and recheck */
    keydata = export_key(key);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    key = get_key_by_uid(ffi, "ecc-p256");
    assert_true(check_key_revoked(key, true));
    assert_true(check_key_revreason(key, revreason));
    assert_rnp_success(rnp_key_is_compromised(key, &compromised));
    assert_true(compromised);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(newffi);
    /* Attempt to revoke primary key using the subkey without designated revoker */
    key = get_key_by_uid(ffi, "ecc-25519");
    revoker = get_key_by_fp(ffi, "6D207DCC0AC281DBFC285785573436C231AE6338");
    assert_rnp_success(rnp_key_revocation_signature_create(revoker, key, &newsig));
    assert_rnp_success(rnp_key_signature_set_revocation_reason(newsig, revcode, revreason));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    assert_true(check_key_revoked(key, false));
    rnp_key_handle_destroy(revoker);
    /* Export and recheck */
    keydata = export_key(key);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    key = get_key_by_uid(newffi, "ecc-25519");
    assert_true(check_key_revoked(key, false));
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(newffi);
    /* Attempt to revoke primary key using the subkey with designated revoker */
    rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET);
    assert_true(
      import_all_keys(ffi, "data/test_stream_key_load/ecc-25519-subkey-revoker.asc"));
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-25519-2subs-sec.asc"));
    key = get_key_by_uid(ffi, "ecc-25519");
    revoker = get_key_by_fp(ffi, "6D207DCC0AC281DBFC285785573436C231AE6338");
    assert_rnp_success(rnp_key_revocation_signature_create(revoker, key, &newsig));
    assert_rnp_success(rnp_key_signature_set_revocation_reason(newsig, revcode, revreason));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    assert_true(check_key_revoked(key, true));
    assert_true(check_key_revreason(key, revreason));
    assert_rnp_success(rnp_key_is_compromised(key, &compromised));
    assert_true(compromised);
    rnp_key_handle_destroy(revoker);
    /* Export and recheck */
    keydata = export_key(key);
    rnp_key_handle_destroy(key);
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    key = get_key_by_uid(newffi, "ecc-25519");
    assert_true(check_key_revoked(key, true));
    assert_true(check_key_revreason(key, revreason));
    assert_rnp_success(rnp_key_is_compromised(key, &compromised));
    assert_true(compromised);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(newffi);
    /* Attempt to revoke signing subkey using itself as revoker */
    rnp_unload_keys(ffi, RNP_KEY_UNLOAD_PUBLIC | RNP_KEY_UNLOAD_SECRET);
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-25519-2subs-sec.asc"));
    key = get_key_by_fp(ffi, "6D207DCC0AC281DBFC285785573436C231AE6338");
    assert_rnp_success(rnp_key_revocation_signature_create(key, NULL, &newsig));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    assert_true(check_key_revoked(key, false));
    rnp_signature_handle_destroy(newsig);
    assert_rnp_success(rnp_key_get_signature_at(key, 0, &newsig));
    assert_true(check_sig_type(newsig, "subkey revocation"));
    assert_rnp_success(rnp_signature_get_revocation_reason(newsig, &sigcode, &sigreason));
    assert_string_equal(sigcode, "");
    assert_string_equal(sigreason, "");
    rnp_buffer_destroy(sigcode);
    rnp_buffer_destroy(sigreason);
    rnp_signature_handle_destroy(newsig);
    rnp_key_handle_destroy(key);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_create_self_certification_signature)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-p256-sec.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    /* Create self certification with default type */
    rnp_signature_handle_t newsig = NULL;
    assert_rnp_failure(
      rnp_key_certification_create(NULL, uid, RNP_CERTIFICATION_POSITIVE, &newsig));
    assert_rnp_failure(
      rnp_key_certification_create(key, NULL, RNP_CERTIFICATION_POSITIVE, &newsig));
    assert_rnp_failure(
      rnp_key_certification_create(key, uid, RNP_CERTIFICATION_POSITIVE, NULL));
    assert_rnp_failure(rnp_key_certification_create(key, uid, "wrong", &newsig));
    assert_rnp_success(rnp_key_certification_create(key, uid, NULL, &newsig));
    const char *hash = "SHA384";
    assert_rnp_success(rnp_key_signature_set_hash(newsig, hash));
    assert_int_equal(rnp_signature_is_valid(newsig, 0), RNP_ERROR_BAD_PARAMETERS);
    assert_rnp_failure(rnp_key_signature_sign(newsig));
    rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password");
    assert_rnp_success(rnp_key_signature_sign(newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_rnp_success(rnp_signature_is_valid(newsig, RNP_SIGNATURE_REVALIDATE));
    rnp_signature_handle_destroy(newsig);
    /* Create self certification with non-default type */
    assert_rnp_success(
      rnp_key_certification_create(key, uid, RNP_CERTIFICATION_PERSONA, &newsig));
    const char *hash2 = "SHA512";
    assert_rnp_success(rnp_key_signature_set_hash(newsig, hash2));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    /* Check certification parameters */
    size_t sigs = 0;
    assert_rnp_success(rnp_uid_get_signature_count(uid, &sigs));
    assert_int_equal(sigs, 3);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, hash));
    assert_true(check_sig_type(newsig, "certification (positive)"));
    rnp_signature_handle_destroy(newsig);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 2, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, hash2));
    assert_true(check_sig_type(newsig, "certification (persona)"));
    rnp_signature_handle_destroy(newsig);
    rnp_uid_handle_destroy(uid);
    /* Export key and make sure data is saved */
    auto keydata = export_key(key, true);
    rnp_key_handle_destroy(key);
    rnp_ffi_t newffi = NULL;
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    assert_rnp_success(rnp_locate_key(newffi, "userid", "ecc-p256", &key));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_get_signature_count(uid, &sigs));
    assert_int_equal(sigs, 3);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, hash));
    assert_true(check_sig_type(newsig, "certification (positive)"));
    rnp_signature_handle_destroy(newsig);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 2, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, hash2));
    assert_true(check_sig_type(newsig, "certification (persona)"));
    rnp_signature_handle_destroy(newsig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(newffi);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_create_key_certification_signature)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-p256-sec.asc"));
    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-25519-sec.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    rnp_key_handle_t signer = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-25519", &signer));
    /* Create other key certification with default type */
    rnp_signature_handle_t newsig = NULL;
    assert_rnp_success(rnp_key_certification_create(signer, uid, NULL, &newsig));
    const char *hash = "SHA384";
    assert_rnp_success(rnp_key_signature_set_hash(newsig, hash));
    assert_rnp_failure(rnp_key_signature_sign(newsig));
    rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password");
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    /* Create other key certification with non-default type */
    assert_rnp_success(
      rnp_key_certification_create(signer, uid, RNP_CERTIFICATION_CASUAL, &newsig));
    const char *hash2 = "SHA512";
    assert_rnp_success(rnp_key_signature_set_hash(newsig, hash2));
    assert_rnp_failure(rnp_key_signature_set_trust_level(NULL, 1, 120));
    assert_rnp_success(rnp_key_signature_set_trust_level(newsig, 1, 120));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    /* Check certification parameters */
    size_t sigs = 0;
    assert_rnp_success(rnp_uid_get_signature_count(uid, &sigs));
    assert_int_equal(sigs, 3);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, hash));
    assert_true(check_sig_type(newsig, "certification (generic)"));
    uint8_t level = 255;
    uint8_t amount = 255;
    assert_rnp_failure(rnp_signature_get_trust_level(NULL, NULL, NULL));
    assert_rnp_success(rnp_signature_get_trust_level(newsig, NULL, NULL));
    assert_rnp_success(rnp_signature_get_trust_level(newsig, &level, NULL));
    assert_int_equal(level, 0);
    assert_rnp_success(rnp_signature_get_trust_level(newsig, NULL, &amount));
    assert_int_equal(amount, 0);
    level = amount = 255;
    assert_rnp_success(rnp_signature_get_trust_level(newsig, &level, &amount));
    assert_int_equal(level, 0);
    assert_int_equal(amount, 0);
    rnp_signature_handle_destroy(newsig);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 2, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_rnp_failure(rnp_key_signature_set_hash(newsig, hash));
    assert_true(check_sig_hash(newsig, hash2));
    assert_true(check_sig_type(newsig, "certification (casual)"));
    assert_rnp_failure(rnp_key_signature_set_trust_level(newsig, 1, 60));
    assert_rnp_success(rnp_signature_get_trust_level(newsig, &level, &amount));
    assert_int_equal(level, 1);
    assert_int_equal(amount, 120);
    rnp_signature_handle_destroy(newsig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(signer);
    /* Export key and make sure data is saved */
    auto keydata = export_key(key, true);
    rnp_key_handle_destroy(key);
    rnp_ffi_t newffi = NULL;
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    assert_rnp_success(rnp_locate_key(newffi, "userid", "ecc-p256", &key));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_get_signature_count(uid, &sigs));
    assert_int_equal(sigs, 3);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &newsig));
    assert_int_equal(rnp_signature_is_valid(newsig, 0), RNP_ERROR_KEY_NOT_FOUND);
    size_t errors = 0;
    assert_rnp_success(rnp_signature_error_count(newsig, &errors));
    assert_int_equal(errors, 1);
    rnp_result_t error = 0;
    assert_rnp_success(rnp_signature_error_at(newsig, 0, &error));
    assert_int_equal(error, RNP_ERROR_SIG_NO_SIGNER_KEY);
    assert_true(import_all_keys(newffi, "data/test_stream_key_load/ecc-25519-pub.asc"));
    assert_rnp_success(rnp_signature_is_valid(newsig, RNP_SIGNATURE_REVALIDATE));
    assert_true(check_sig_hash(newsig, hash));
    assert_true(check_sig_type(newsig, "certification (generic)"));
    rnp_signature_handle_destroy(newsig);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 2, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, hash2));
    assert_true(check_sig_type(newsig, "certification (casual)"));
    rnp_signature_handle_destroy(newsig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(newffi);

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_ffi_key_self_certification_features)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    rnp_ffi_set_pass_provider(ffi, ffi_string_password_provider, (void *) "password");

    assert_true(import_all_keys(ffi, "data/test_stream_key_load/ecc-p256-sec.asc"));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_locate_key(ffi, "userid", "ecc-p256", &key));
    uint32_t sig_creation = 0;
    assert_rnp_success(rnp_key_get_creation(key, &sig_creation));
    const uint32_t sig_diff = 1000;
    sig_creation += sig_diff;
    rnp_uid_handle_t uid = NULL;
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    /* Create key certification with default fields to check later */
    rnp_signature_handle_t newsig = NULL;
    assert_rnp_success(rnp_key_certification_create(key, uid, NULL, &newsig));
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    /* Create key certification and set different fields to check later */
    assert_rnp_success(rnp_key_certification_create(key, uid, NULL, &newsig));
    /* Signature creation time */
    assert_rnp_failure(rnp_key_signature_set_creation(NULL, 0));
    assert_rnp_success(rnp_key_signature_set_creation(newsig, sig_creation));
    /* Signature hash algorithm */
    const char *hash = "SHA384";
    assert_rnp_success(rnp_key_signature_set_hash(newsig, hash));
    /* Key flags */
    assert_rnp_failure(rnp_key_signature_set_key_flags(NULL, RNP_KEY_USAGE_CERTIFY));
    assert_rnp_failure(rnp_key_signature_set_key_flags(newsig, 0x100 | RNP_KEY_USAGE_CERTIFY));
    assert_rnp_success(rnp_key_signature_set_key_flags(newsig, RNP_KEY_USAGE_CERTIFY));
    /* Key expiration */
    uint32_t expiry = 50 * 365 * 24 * 60 * 60;
    assert_rnp_failure(rnp_key_signature_set_key_expiration(NULL, expiry));
    assert_rnp_success(rnp_key_signature_set_key_expiration(newsig, 100000));
    assert_rnp_success(rnp_key_signature_set_key_expiration(newsig, expiry));
    /* Primary user id */
    assert_rnp_failure(rnp_key_signature_set_primary_uid(NULL, true));
    assert_rnp_success(rnp_key_signature_set_primary_uid(newsig, false));
    assert_rnp_success(rnp_key_signature_set_primary_uid(newsig, true));
    /* Key server */
    const char *key_serv = "https://key-server/";
    assert_rnp_failure(rnp_key_signature_set_key_server(NULL, key_serv));
    assert_rnp_success(rnp_key_signature_set_key_server(newsig, "wrong"));
    assert_rnp_success(rnp_key_signature_set_key_server(newsig, NULL));
    assert_rnp_success(rnp_key_signature_set_key_server(newsig, "wrong2"));
    assert_rnp_success(rnp_key_signature_set_key_server(newsig, key_serv));
    /* Key server prefs */
    assert_rnp_failure(rnp_key_signature_set_key_server_prefs(NULL, RNP_KEY_SERVER_NO_MODIFY));
    assert_rnp_failure(
      rnp_key_signature_set_key_server_prefs(newsig, 0x101 | RNP_KEY_SERVER_NO_MODIFY));
    assert_rnp_success(rnp_key_signature_set_key_server_prefs(newsig, 0));
    assert_rnp_success(
      rnp_key_signature_set_key_server_prefs(newsig, RNP_KEY_SERVER_NO_MODIFY));
    /* Key features */
    uint32_t features = RNP_KEY_FEATURE_MDC | RNP_KEY_FEATURE_AEAD | RNP_KEY_FEATURE_V5;
    assert_rnp_failure(rnp_key_signature_set_features(NULL, features));
    assert_rnp_failure(rnp_key_signature_set_features(newsig, 0x123));
    assert_rnp_success(rnp_key_signature_set_features(newsig, RNP_KEY_FEATURE_MDC));
    assert_rnp_success(rnp_key_signature_set_features(newsig, features));
    /* Preferred symmetric algorithms */
    assert_rnp_failure(rnp_key_signature_add_preferred_alg(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_add_preferred_alg(NULL, RNP_ALGNAME_AES_256));
    assert_rnp_failure(rnp_key_signature_add_preferred_alg(newsig, RNP_ALGNAME_SHA256));
    assert_rnp_failure(rnp_key_signature_add_preferred_alg(newsig, RNP_ALGNAME_SHA256));
    assert_rnp_success(rnp_key_signature_add_preferred_alg(newsig, RNP_ALGNAME_AES_256));
    assert_rnp_success(rnp_key_signature_add_preferred_alg(newsig, RNP_ALGNAME_CAMELLIA_192));
    assert_rnp_success(rnp_key_signature_add_preferred_alg(newsig, RNP_ALGNAME_IDEA));
    /* Preferred hash algorithms */
    assert_rnp_failure(rnp_key_signature_add_preferred_hash(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_add_preferred_hash(NULL, RNP_ALGNAME_SHA256));
    assert_rnp_failure(rnp_key_signature_add_preferred_hash(newsig, RNP_ALGNAME_AES_256));
    assert_rnp_success(rnp_key_signature_add_preferred_hash(newsig, RNP_ALGNAME_SHA256));
    assert_rnp_success(rnp_key_signature_add_preferred_hash(newsig, RNP_ALGNAME_SHA3_512));
    assert_rnp_success(rnp_key_signature_add_preferred_hash(newsig, RNP_ALGNAME_SM3));
    /* Preferred compression algorithms */
    assert_rnp_failure(rnp_key_signature_add_preferred_zalg(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_add_preferred_zalg(NULL, RNP_ALGNAME_BZIP2));
    assert_rnp_failure(rnp_key_signature_add_preferred_zalg(newsig, RNP_ALGNAME_SHA256));
    assert_rnp_success(rnp_key_signature_add_preferred_zalg(newsig, RNP_ALGNAME_ZLIB));
    assert_rnp_success(rnp_key_signature_add_preferred_zalg(newsig, RNP_ALGNAME_BZIP2));
    /* Sign */
    assert_rnp_success(rnp_key_signature_sign(newsig));
    rnp_signature_handle_destroy(newsig);
    /* Check certification parameters */
    size_t sigs = 0;
    assert_rnp_success(rnp_uid_get_signature_count(uid, &sigs));
    assert_int_equal(sigs, 3);
    /* First signature, available after the load */
    uint32_t check = 0;
    bool     primary = false;
    assert_rnp_success(rnp_uid_get_signature_at(uid, 0, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, RNP_ALGNAME_SHA256));
    assert_true(check_sig_type(newsig, "certification (positive)"));
    assert_rnp_success(rnp_signature_get_creation(newsig, &check));
    assert_int_equal(check, 1549119463);
    assert_rnp_success(rnp_signature_get_key_expiration(newsig, &check));
    assert_int_equal(check, 0);
    assert_rnp_success(rnp_signature_get_primary_uid(newsig, &primary));
    assert_false(primary);
    assert_rnp_success(rnp_signature_get_features(newsig, &check));
    assert_int_equal(check, 3);
    assert_rnp_success(rnp_signature_get_key_flags(newsig, &check));
    assert_int_equal(check, RNP_KEY_USAGE_CERTIFY | RNP_KEY_USAGE_SIGN);
    assert_rnp_success(rnp_signature_get_key_server_prefs(newsig, &check));
    assert_int_equal(check, RNP_KEY_SERVER_NO_MODIFY);
    size_t count = 0;
    assert_rnp_success(rnp_signature_get_preferred_alg_count(newsig, &count));
    assert_int_equal(count, 4);
    char *alg = NULL;
    assert_rnp_success(rnp_signature_get_preferred_alg(newsig, 0, &alg));
    assert_string_equal(alg, "AES256");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_alg(newsig, 1, &alg));
    assert_string_equal(alg, "AES192");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_alg(newsig, 2, &alg));
    assert_string_equal(alg, "AES128");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_alg(newsig, 3, &alg));
    assert_string_equal(alg, "TRIPLEDES");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_hash_count(newsig, &count));
    assert_int_equal(count, 5);
    assert_rnp_success(rnp_signature_get_preferred_hash(newsig, 0, &alg));
    assert_string_equal(alg, "SHA512");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_hash(newsig, 1, &alg));
    assert_string_equal(alg, "SHA384");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_hash(newsig, 2, &alg));
    assert_string_equal(alg, "SHA256");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_hash(newsig, 3, &alg));
    assert_string_equal(alg, "SHA224");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_hash(newsig, 4, &alg));
    assert_string_equal(alg, "SHA1");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_zalg_count(newsig, &count));
    assert_int_equal(count, 3);
    assert_rnp_success(rnp_signature_get_preferred_zalg(newsig, 0, &alg));
    assert_string_equal(alg, "ZLIB");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_zalg(newsig, 1, &alg));
    assert_string_equal(alg, "BZip2");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_zalg(newsig, 2, &alg));
    assert_string_equal(alg, "ZIP");
    rnp_buffer_destroy(alg);
    rnp_sig_subpacket_t subpkt = NULL;
    assert_rnp_success(rnp_signature_subpacket_count(newsig, &count));
    assert_int_equal(count, 10);
    assert_rnp_success(rnp_signature_subpacket_at(newsig, 0, &subpkt));
    uint8_t type = 0;
    bool    hashed = false;
    bool    critical = false;
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, &type, &hashed, &critical));
    assert_int_equal(type, 27);
    assert_true(hashed);
    assert_false(critical);
    uint8_t *data = NULL;
    size_t   size = 0;
    assert_rnp_success(rnp_signature_subpacket_data(subpkt, &data, &size));
    assert_int_equal(size, 1);
    assert_int_equal(data[0], 3);
    rnp_buffer_destroy(data);
    rnp_signature_subpacket_destroy(subpkt);
    assert_rnp_success(rnp_signature_subpacket_at(newsig, 9, &subpkt));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, &type, &hashed, &critical));
    assert_int_equal(type, 16);
    assert_false(hashed);
    assert_false(critical);
    assert_rnp_success(rnp_signature_subpacket_data(subpkt, &data, &size));
    assert_int_equal(size, 8);
    assert_int_equal(data[0], 0x23);
    assert_int_equal(data[7], 0x27);
    rnp_buffer_destroy(data);
    rnp_signature_subpacket_destroy(subpkt);
    rnp_signature_handle_destroy(newsig);
    /* First newly added signature with defaults */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, DEFAULT_HASH_ALG));
    assert_true(check_sig_type(newsig, "certification (positive)"));
    uint32_t now = ::time(NULL);
    assert_rnp_failure(rnp_signature_get_creation(NULL, &check));
    assert_rnp_failure(rnp_signature_get_creation(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_set_creation(newsig, 0));
    assert_rnp_success(rnp_signature_get_creation(newsig, &check));
    assert_true(check >= now - 10);
    assert_rnp_failure(rnp_signature_get_key_expiration(NULL, &check));
    assert_rnp_failure(rnp_signature_get_key_expiration(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_set_key_expiration(newsig, 10000));
    assert_rnp_success(rnp_signature_get_key_expiration(newsig, &check));
    assert_int_equal(check, 0);
    assert_rnp_failure(rnp_signature_get_primary_uid(NULL, &primary));
    assert_rnp_failure(rnp_signature_get_primary_uid(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_set_primary_uid(newsig, true));
    assert_rnp_success(rnp_signature_get_primary_uid(newsig, &primary));
    assert_false(primary);
    char *strcheck = NULL;
    assert_rnp_failure(rnp_signature_get_key_server(NULL, &strcheck));
    assert_rnp_failure(rnp_signature_get_key_server(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_set_key_server(newsig, "server"));
    assert_rnp_success(rnp_signature_get_key_server(newsig, &strcheck));
    assert_string_equal(strcheck, "");
    rnp_buffer_destroy(strcheck);
    assert_rnp_failure(rnp_signature_get_features(NULL, &check));
    assert_rnp_failure(rnp_signature_get_features(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_set_features(newsig, features));
    assert_rnp_success(rnp_signature_get_features(newsig, &check));
    assert_int_equal(check, 0);
    assert_rnp_failure(rnp_signature_get_key_flags(NULL, &check));
    assert_rnp_failure(rnp_signature_get_key_flags(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_set_key_flags(newsig, RNP_KEY_USAGE_CERTIFY));
    assert_rnp_success(rnp_signature_get_features(newsig, &check));
    assert_int_equal(check, 0);
    assert_rnp_failure(rnp_signature_get_key_server_prefs(NULL, &check));
    assert_rnp_failure(rnp_signature_get_key_server_prefs(newsig, NULL));
    assert_rnp_failure(
      rnp_key_signature_set_key_server_prefs(newsig, RNP_KEY_SERVER_NO_MODIFY));
    assert_rnp_success(rnp_signature_get_key_server_prefs(newsig, &check));
    assert_int_equal(check, 0);
    assert_rnp_failure(rnp_signature_get_preferred_alg_count(NULL, &count));
    assert_rnp_failure(rnp_signature_get_preferred_alg_count(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_add_preferred_alg(newsig, RNP_ALGNAME_AES_256));
    assert_rnp_success(rnp_signature_get_preferred_alg_count(newsig, &count));
    assert_int_equal(count, 0);
    assert_rnp_failure(rnp_signature_get_preferred_alg(NULL, 0, &alg));
    assert_rnp_failure(rnp_signature_get_preferred_alg(newsig, 0, NULL));
    assert_rnp_failure(rnp_signature_get_preferred_alg(newsig, 0, &alg));
    assert_rnp_failure(rnp_signature_get_preferred_hash_count(NULL, &count));
    assert_rnp_failure(rnp_signature_get_preferred_hash_count(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_add_preferred_hash(newsig, RNP_ALGNAME_SHA512));
    assert_rnp_success(rnp_signature_get_preferred_hash_count(newsig, &count));
    assert_int_equal(count, 0);
    assert_rnp_failure(rnp_signature_get_preferred_hash(NULL, 0, &alg));
    assert_rnp_failure(rnp_signature_get_preferred_hash(newsig, 0, NULL));
    assert_rnp_failure(rnp_signature_get_preferred_hash(newsig, 0, &alg));
    assert_rnp_failure(rnp_signature_get_preferred_zalg_count(NULL, &count));
    assert_rnp_failure(rnp_signature_get_preferred_zalg_count(newsig, NULL));
    assert_rnp_failure(rnp_key_signature_add_preferred_zalg(newsig, RNP_ALGNAME_BZIP2));
    assert_rnp_success(rnp_signature_get_preferred_zalg_count(newsig, &count));
    assert_int_equal(count, 0);
    assert_rnp_failure(rnp_signature_get_preferred_zalg(NULL, 0, &alg));
    assert_rnp_failure(rnp_signature_get_preferred_zalg(newsig, 0, NULL));
    assert_rnp_failure(rnp_signature_get_preferred_zalg(newsig, 0, &alg));

    assert_rnp_failure(rnp_signature_subpacket_count(NULL, &count));
    assert_rnp_failure(rnp_signature_subpacket_count(newsig, NULL));
    assert_rnp_success(rnp_signature_subpacket_count(newsig, &count));
    assert_int_equal(count, 3);
    assert_rnp_failure(rnp_signature_subpacket_at(NULL, 0, &subpkt));
    assert_rnp_failure(rnp_signature_subpacket_at(newsig, 0, NULL));
    assert_int_equal(rnp_signature_subpacket_at(newsig, 20, &subpkt), RNP_ERROR_NOT_FOUND);
    assert_rnp_success(rnp_signature_subpacket_at(newsig, 0, &subpkt));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, NULL, NULL, NULL));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, &type, NULL, NULL));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, NULL, &hashed, NULL));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, NULL, NULL, &critical));
    assert_int_equal(type, 33);
    assert_true(hashed);
    assert_false(critical);
    assert_rnp_failure(rnp_signature_subpacket_data(NULL, &data, &size));
    assert_rnp_failure(rnp_signature_subpacket_data(subpkt, NULL, &size));
    assert_rnp_failure(rnp_signature_subpacket_data(subpkt, &data, NULL));
    assert_rnp_success(rnp_signature_subpacket_data(subpkt, &data, &size));
    assert_int_equal(size, 21);
    assert_int_equal(data[0], 0x04);
    assert_int_equal(data[1], 0xb5);
    assert_int_equal(data[20], 0x27);
    rnp_buffer_destroy(data);
    rnp_signature_subpacket_destroy(subpkt);
    assert_rnp_success(rnp_signature_subpacket_at(newsig, 1, &subpkt));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, &type, &hashed, &critical));
    assert_int_equal(type, 2);
    assert_true(hashed);
    assert_false(critical);
    assert_rnp_success(rnp_signature_subpacket_data(subpkt, &data, &size));
    assert_int_equal(size, 4);
    rnp_buffer_destroy(data);
    rnp_signature_subpacket_destroy(subpkt);
    assert_rnp_success(rnp_signature_subpacket_at(newsig, 2, &subpkt));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, &type, &hashed, &critical));
    assert_int_equal(type, 16);
    assert_false(hashed);
    assert_false(critical);
    assert_rnp_success(rnp_signature_subpacket_data(subpkt, &data, &size));
    assert_int_equal(size, 8);
    assert_int_equal(data[0], 0x23);
    rnp_buffer_destroy(data);
    rnp_signature_subpacket_destroy(subpkt);
    assert_rnp_failure(rnp_signature_subpacket_find(NULL, 33, true, 0, &subpkt));
    assert_rnp_failure(rnp_signature_subpacket_find(newsig, 33, true, 0, NULL));
    assert_int_equal(rnp_signature_subpacket_find(newsig, 33, true, 1, &subpkt),
                     RNP_ERROR_NOT_FOUND);
    assert_int_equal(rnp_signature_subpacket_find(newsig, 18, true, 0, &subpkt),
                     RNP_ERROR_NOT_FOUND);
    assert_rnp_success(rnp_signature_subpacket_find(newsig, 33, true, 0, &subpkt));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, &type, &hashed, &critical));
    assert_int_equal(type, 33);
    assert_true(hashed);
    assert_false(critical);
    rnp_signature_subpacket_destroy(subpkt);
    assert_rnp_success(rnp_signature_subpacket_find(newsig, 16, false, 0, &subpkt));
    assert_rnp_success(rnp_signature_subpacket_info(subpkt, &type, &hashed, &critical));
    assert_int_equal(type, 16);
    assert_false(hashed);
    assert_false(critical);
    rnp_signature_subpacket_destroy(subpkt);
    rnp_signature_handle_destroy(newsig);
    /* Second newly added signature with customized parameters */
    assert_rnp_success(rnp_uid_get_signature_at(uid, 2, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_type(newsig, "certification (positive)"));
    assert_rnp_success(rnp_signature_get_creation(newsig, &check));
    assert_int_equal(check, sig_creation);
    assert_true(check_sig_hash(newsig, hash));
    assert_rnp_success(rnp_signature_get_key_expiration(newsig, &check));
    assert_int_equal(check, expiry);
    assert_rnp_success(rnp_signature_get_primary_uid(newsig, &primary));
    assert_true(primary);
    assert_rnp_success(rnp_signature_get_key_server(newsig, &strcheck));
    assert_string_equal(strcheck, key_serv);
    rnp_buffer_destroy(strcheck);
    assert_rnp_success(rnp_signature_get_key_server_prefs(newsig, &check));
    assert_int_equal(check, RNP_KEY_SERVER_NO_MODIFY);
    assert_rnp_success(rnp_signature_get_features(newsig, &check));
    assert_int_equal(check, features);
    assert_rnp_success(rnp_signature_get_key_flags(newsig, &check));
    assert_int_equal(check, RNP_KEY_USAGE_CERTIFY);
    assert_rnp_success(rnp_signature_get_preferred_alg_count(newsig, &count));
    assert_int_equal(count, 3);
    assert_rnp_success(rnp_signature_get_preferred_alg(newsig, 0, &alg));
    assert_string_equal(alg, "AES256");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_alg(newsig, 1, &alg));
    assert_string_equal(alg, "CAMELLIA192");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_alg(newsig, 2, &alg));
    assert_string_equal(alg, "IDEA");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_hash_count(newsig, &count));
    assert_int_equal(count, 3);
    assert_rnp_success(rnp_signature_get_preferred_hash(newsig, 0, &alg));
    assert_string_equal(alg, "SHA256");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_hash(newsig, 1, &alg));
    assert_string_equal(alg, "SHA3-512");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_hash(newsig, 2, &alg));
    assert_string_equal(alg, "SM3");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_zalg_count(newsig, &count));
    assert_int_equal(count, 2);
    assert_rnp_success(rnp_signature_get_preferred_zalg(newsig, 0, &alg));
    assert_string_equal(alg, "ZLIB");
    rnp_buffer_destroy(alg);
    assert_rnp_success(rnp_signature_get_preferred_zalg(newsig, 1, &alg));
    assert_string_equal(alg, "BZip2");
    rnp_buffer_destroy(alg);
    rnp_signature_handle_destroy(newsig);
    rnp_uid_handle_destroy(uid);
    /* Export key and make sure data is saved */
    auto keydata = export_key(key, true);
    rnp_key_handle_destroy(key);
    rnp_ffi_t newffi = NULL;
    assert_rnp_success(rnp_ffi_create(&newffi, "GPG", "GPG"));
    assert_true(import_all_keys(newffi, keydata.data(), keydata.size()));
    assert_rnp_success(rnp_locate_key(newffi, "userid", "ecc-p256", &key));
    assert_rnp_success(rnp_key_get_uid_handle_at(key, 0, &uid));
    assert_rnp_success(rnp_uid_get_signature_count(uid, &sigs));
    assert_int_equal(sigs, 3);
    assert_rnp_success(rnp_uid_get_signature_at(uid, 1, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, DEFAULT_HASH_ALG));
    assert_true(check_sig_type(newsig, "certification (positive)"));
    assert_rnp_success(rnp_signature_get_creation(newsig, &check));
    assert_true(check >= now - 20);
    rnp_signature_handle_destroy(newsig);

    assert_rnp_success(rnp_uid_get_signature_at(uid, 2, &newsig));
    assert_rnp_success(rnp_signature_is_valid(newsig, 0));
    assert_true(check_sig_hash(newsig, hash));
    assert_true(check_sig_type(newsig, "certification (positive)"));
    assert_rnp_success(rnp_signature_get_key_server(newsig, &strcheck));
    assert_string_equal(strcheck, key_serv);
    assert_rnp_success(rnp_signature_get_key_flags(newsig, &check));
    assert_int_equal(check, RNP_KEY_USAGE_CERTIFY);
    rnp_buffer_destroy(strcheck);
    rnp_signature_handle_destroy(newsig);
    rnp_uid_handle_destroy(uid);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(newffi);

    rnp_ffi_destroy(ffi);
}
