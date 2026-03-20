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

#include <rnp/rnp.h>
#include <string.h>
#include <time.h>

#define RNP_SUCCESS 0

/* sample pass provider implementation, which always return 'password' */
static bool
example_pass_provider(rnp_ffi_t        ffi,
                      void *           app_ctx,
                      rnp_key_handle_t key,
                      const char *     pgp_context,
                      char             buf[],
                      size_t           buf_len)
{
    strncpy(buf, "password", buf_len);
    return true;
}

static int
ffi_sign()
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      keyfile = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_sign_t    sign = NULL;
    rnp_key_handle_t key = NULL;
    const char *     message = "RNP signing sample message";
    int              result = 1;

    /* initialize FFI object */
    if (rnp_ffi_create(&ffi, "GPG", "GPG") != RNP_SUCCESS) {
        return result;
    }

    /* load secret keyring, as it is required for signing. However, you may need to load public
     * keyring as well to validate key's signatures. */
    if (rnp_input_from_path(&keyfile, "secring.pgp") != RNP_SUCCESS) {
        fprintf(stdout, "failed to open secring.pgp. Did you run ./generate sample?\n");
        goto finish;
    }

    /* we may use RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_PUBLIC_KEYS as well */
    if (rnp_load_keys(ffi, "GPG", keyfile, RNP_LOAD_SAVE_SECRET_KEYS) != RNP_SUCCESS) {
        fprintf(stdout, "failed to read secring.pgp\n");
        goto finish;
    }
    rnp_input_destroy(keyfile);
    keyfile = NULL;

    /* set the password provider - we'll need password to unlock secret keys */
    rnp_ffi_set_pass_provider(ffi, example_pass_provider, NULL);

    /* create file input and memory output objects for the encrypted message and decrypted
     * message */
    if (rnp_input_from_memory(&input, (uint8_t *) message, strlen(message), false) !=
        RNP_SUCCESS) {
        fprintf(stdout, "failed to create input object\n");
        goto finish;
    }

    if (rnp_output_to_path(&output, "signed.asc") != RNP_SUCCESS) {
        fprintf(stdout, "failed to create output object\n");
        goto finish;
    }

    /* initialize and configure sign operation, use
     * rnp_op_sign_create_cleartext/rnp_op_sign_create_detached for cleartext or detached
     * signature. */
    if (rnp_op_sign_create(&sign, ffi, input, output) != RNP_SUCCESS) {
        fprintf(stdout, "failed to create sign operation\n");
        goto finish;
    }

    /* armor, file name, compression */
    rnp_op_sign_set_armor(sign, true);
    rnp_op_sign_set_file_name(sign, "message.txt");
    rnp_op_sign_set_file_mtime(sign, (uint32_t) time(NULL));
    rnp_op_sign_set_compression(sign, "ZIP", 6);
    /* signatures creation time - by default will be set to the current time as well */
    rnp_op_sign_set_creation_time(sign, (uint32_t) time(NULL));
    /* signatures expiration time - by default will be 0, i.e. never expire */
    rnp_op_sign_set_expiration_time(sign, 365 * 24 * 60 * 60);
    /* set hash algorithm - should be compatible for all signatures */
    rnp_op_sign_set_hash(sign, RNP_ALGNAME_SHA256);

    /* now add signatures. First locate the signing key, then add and setup signature */
    /* RSA signature */
    if (rnp_locate_key(ffi, "userid", "rsa@key", &key) != RNP_SUCCESS) {
        fprintf(stdout, "failed to locate signing key rsa@key.\n");
        goto finish;
    }

    /* we do not need pointer to the signature so passing NULL as the last parameter */
    if (rnp_op_sign_add_signature(sign, key, NULL) != RNP_SUCCESS) {
        fprintf(stdout, "failed to add signature for key rsa@key.\n");
        goto finish;
    }

    /* do not forget to destroy key handle */
    rnp_key_handle_destroy(key);
    key = NULL;

    /* EdDSA signature */
    if (rnp_locate_key(ffi, "userid", "25519@key", &key) != RNP_SUCCESS) {
        fprintf(stdout, "failed to locate signing key 25519@key.\n");
        goto finish;
    }

    if (rnp_op_sign_add_signature(sign, key, NULL) != RNP_SUCCESS) {
        fprintf(stdout, "failed to add signature for key 25519@key.\n");
        goto finish;
    }

    rnp_key_handle_destroy(key);
    key = NULL;

    /* finally do signing */
    if (rnp_op_sign_execute(sign) != RNP_SUCCESS) {
        fprintf(stdout, "failed to sign\n");
        goto finish;
    }

    fprintf(stdout, "Signing succeeded. See file signed.asc.\n");

    result = 0;
finish:
    rnp_input_destroy(keyfile);
    rnp_key_handle_destroy(key);
    rnp_op_sign_destroy(sign);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_ffi_destroy(ffi);
    return result;
}

int
main(int argc, char **argv)
{
    return ffi_sign();
}
