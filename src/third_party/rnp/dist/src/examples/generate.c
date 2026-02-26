/*
 * Copyright (c) 2018, [Ribose Inc](https://www.ribose.com).
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

#include <string.h>
#include <rnp/rnp.h>

#define RNP_SUCCESS 0

/* RSA key JSON description. 31536000 = 1 year expiration, 15768000 = half year */
const char *RSA_KEY_DESC = "{\
    'primary': {\
        'type': 'RSA',\
        'length': 2048,\
        'userid': 'rsa@key',\
        'expiration': 31536000,\
        'usage': ['sign'],\
        'protection': {\
            'cipher': 'AES256',\
            'hash': 'SHA256'\
        }\
    },\
    'sub': {\
        'type': 'RSA',\
        'length': 2048,\
        'expiration': 15768000,\
        'usage': ['encrypt'],\
        'protection': {\
            'cipher': 'AES256',\
            'hash': 'SHA256'\
        }\
    }\
}";

const char *CURVE_25519_KEY_DESC = "{\
    'primary': {\
        'type': 'EDDSA',\
        'userid': '25519@key',\
        'expiration': 0,\
        'usage': ['sign'],\
        'protection': {\
            'cipher': 'AES256',\
            'hash': 'SHA256'\
        }\
    },\
    'sub': {\
        'type': 'ECDH',\
        'curve': 'Curve25519',\
        'expiration': 15768000,\
        'usage': ['encrypt'],\
        'protection': {\
            'cipher': 'AES256',\
            'hash': 'SHA256'\
        }\
    }\
}";

/* basic pass provider implementation, which always return 'password' for key protection.
You may ask for password via stdin, or choose password based on key properties, whatever else
*/
static bool
example_pass_provider(rnp_ffi_t        ffi,
                      void *           app_ctx,
                      rnp_key_handle_t key,
                      const char *     pgp_context,
                      char             buf[],
                      size_t           buf_len)
{
    if (strcmp(pgp_context, "protect")) {
        return false;
    }

    strncpy(buf, "password", buf_len);
    return true;
}

/* this simple helper function just prints armored key, searched by userid, to stdout */
static bool
ffi_print_key(rnp_ffi_t ffi, const char *uid, bool secret)
{
    rnp_output_t     keydata = NULL;
    rnp_key_handle_t key = NULL;
    uint32_t         flags = RNP_KEY_EXPORT_ARMORED | RNP_KEY_EXPORT_SUBKEYS;
    uint8_t *        buf = NULL;
    size_t           buf_len = 0;
    bool             result = false;

    /* you may search for the key via userid, keyid, fingerprint, grip */
    if (rnp_locate_key(ffi, "userid", uid, &key) != RNP_SUCCESS) {
        return false;
    }

    if (!key) {
        return false;
    }

    /* create in-memory output structure to later use buffer */
    if (rnp_output_to_memory(&keydata, 0) != RNP_SUCCESS) {
        goto finish;
    }

    flags = flags | (secret ? RNP_KEY_EXPORT_SECRET : RNP_KEY_EXPORT_PUBLIC);
    if (rnp_key_export(key, keydata, flags) != RNP_SUCCESS) {
        goto finish;
    }

    /* get key's contents from the output structure */
    if (rnp_output_memory_get_buf(keydata, &buf, &buf_len, false) != RNP_SUCCESS) {
        goto finish;
    }
    fprintf(stdout, "%.*s", (int) buf_len, buf);

    result = true;
finish:
    rnp_key_handle_destroy(key);
    rnp_output_destroy(keydata);
    return result;
}

static bool
ffi_export_key(rnp_ffi_t ffi, const char *uid, bool secret)
{
    rnp_output_t     keyfile = NULL;
    rnp_key_handle_t key = NULL;
    uint32_t         flags = RNP_KEY_EXPORT_ARMORED | RNP_KEY_EXPORT_SUBKEYS;
    char             filename[32] = {0};
    char *           keyid = NULL;
    bool             result = false;

    /* you may search for the key via userid, keyid, fingerprint, grip */
    if (rnp_locate_key(ffi, "userid", uid, &key) != RNP_SUCCESS) {
        return false;
    }

    if (!key) {
        return false;
    }

    /* get key's id and build filename */
    if (rnp_key_get_keyid(key, &keyid) != RNP_SUCCESS) {
        goto finish;
    }
    snprintf(filename, sizeof(filename), "key-%s-%s.asc", keyid, secret ? "sec" : "pub");
    rnp_buffer_destroy(keyid);

    /* create file output structure */
    if (rnp_output_to_path(&keyfile, filename) != RNP_SUCCESS) {
        goto finish;
    }

    flags = flags | (secret ? RNP_KEY_EXPORT_SECRET : RNP_KEY_EXPORT_PUBLIC);
    if (rnp_key_export(key, keyfile, flags) != RNP_SUCCESS) {
        goto finish;
    }

    result = true;
finish:
    rnp_key_handle_destroy(key);
    rnp_output_destroy(keyfile);
    return result;
}

/* this example function generates RSA/RSA and Eddsa/X25519 keypairs */
static int
ffi_generate_keys()
{
    rnp_ffi_t    ffi = NULL;
    rnp_output_t keyfile = NULL;
    char *       key_grips = NULL;
    int          result = 1;

    /* initialize FFI object */
    if (rnp_ffi_create(&ffi, "GPG", "GPG") != RNP_SUCCESS) {
        return result;
    }

    /* set password provider */
    if (rnp_ffi_set_pass_provider(ffi, example_pass_provider, NULL)) {
        goto finish;
    }

    /* generate EDDSA/X25519 keypair */
    if (rnp_generate_key_json(ffi, CURVE_25519_KEY_DESC, &key_grips) != RNP_SUCCESS) {
        fprintf(stdout, "failed to generate eddsa key\n");
        goto finish;
    }

    fprintf(stdout, "Generated 25519 key/subkey:\n%s\n", key_grips);
    /* destroying key_grips buffer is our obligation */
    rnp_buffer_destroy(key_grips);
    key_grips = NULL;

    /* generate RSA keypair */
    if (rnp_generate_key_json(ffi, RSA_KEY_DESC, &key_grips) != RNP_SUCCESS) {
        fprintf(stdout, "failed to generate rsa key\n");
        goto finish;
    }

    fprintf(stdout, "Generated RSA key/subkey:\n%s\n", key_grips);
    rnp_buffer_destroy(key_grips);
    key_grips = NULL;

    /* create file output object and save public keyring with generated keys, overwriting
     * previous file if any. You may use rnp_output_to_memory() here as well. */
    if (rnp_output_to_path(&keyfile, "pubring.pgp") != RNP_SUCCESS) {
        fprintf(stdout, "failed to initialize pubring.pgp writing\n");
        goto finish;
    }

    if (rnp_save_keys(ffi, "GPG", keyfile, RNP_LOAD_SAVE_PUBLIC_KEYS) != RNP_SUCCESS) {
        fprintf(stdout, "failed to save pubring\n");
        goto finish;
    }

    rnp_output_destroy(keyfile);
    keyfile = NULL;

    /* create file output object and save secret keyring with generated keys */
    if (rnp_output_to_path(&keyfile, "secring.pgp") != RNP_SUCCESS) {
        fprintf(stdout, "failed to initialize secring.pgp writing\n");
        goto finish;
    }

    if (rnp_save_keys(ffi, "GPG", keyfile, RNP_LOAD_SAVE_SECRET_KEYS) != RNP_SUCCESS) {
        fprintf(stdout, "failed to save secring\n");
        goto finish;
    }

    rnp_output_destroy(keyfile);
    keyfile = NULL;

    result = 0;
finish:
    rnp_buffer_destroy(key_grips);
    rnp_output_destroy(keyfile);
    rnp_ffi_destroy(ffi);
    return result;
}

static int
ffi_output_keys()
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t keyfile = NULL;
    int         result = 2;

    /* initialize FFI object */
    if (rnp_ffi_create(&ffi, "GPG", "GPG") != RNP_SUCCESS) {
        return result;
    }

    /* load keyrings */
    if (rnp_input_from_path(&keyfile, "pubring.pgp") != RNP_SUCCESS) {
        fprintf(stdout, "failed to open pubring.pgp\n");
        goto finish;
    }

    /* actually, we may use 0 instead of RNP_LOAD_SAVE_PUBLIC_KEYS, to not check key types */
    if (rnp_load_keys(ffi, "GPG", keyfile, RNP_LOAD_SAVE_PUBLIC_KEYS) != RNP_SUCCESS) {
        fprintf(stdout, "failed to read pubring.pgp\n");
        goto finish;
    }
    rnp_input_destroy(keyfile);
    keyfile = NULL;

    if (rnp_input_from_path(&keyfile, "secring.pgp") != RNP_SUCCESS) {
        fprintf(stdout, "failed to open secring.pgp\n");
        goto finish;
    }

    if (rnp_load_keys(ffi, "GPG", keyfile, RNP_LOAD_SAVE_SECRET_KEYS) != RNP_SUCCESS) {
        fprintf(stdout, "failed to read secring.pgp\n");
        goto finish;
    }
    rnp_input_destroy(keyfile);
    keyfile = NULL;

    /* print armored keys to the stdout */
    if (!ffi_print_key(ffi, "rsa@key", false) || !ffi_print_key(ffi, "rsa@key", true) ||
        !ffi_print_key(ffi, "25519@key", false) || !ffi_print_key(ffi, "25519@key", true)) {
        fprintf(stdout, "failed to print armored key(s)\n");
        goto finish;
    }

    /* write armored keys to the files, named key-<keyid>-pub.asc/named key-<keyid>-sec.asc */
    if (!ffi_export_key(ffi, "rsa@key", false) || !ffi_export_key(ffi, "rsa@key", true) ||
        !ffi_export_key(ffi, "25519@key", false) || !ffi_export_key(ffi, "25519@key", true)) {
        fprintf(stdout, "failed to write armored key(s) to file\n");
        goto finish;
    }

    result = 0;
finish:
    rnp_input_destroy(keyfile);
    rnp_ffi_destroy(ffi);
    return result;
}

int
main(int argc, char **argv)
{
    int res = ffi_generate_keys();
    if (res) {
        return res;
    }
    res = ffi_output_keys();
    return res;
}
