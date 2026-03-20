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
#include <time.h>
#include <rnp/rnp.h>

#define RNP_SUCCESS 0

static int
ffi_encrypt()
{
    rnp_ffi_t        ffi = NULL;
    rnp_op_encrypt_t encrypt = NULL;
    rnp_key_handle_t key = NULL;
    rnp_input_t      keyfile = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    const char *     message = "RNP encryption sample message";
    int              result = 1;

    /* initialize FFI object */
    if (rnp_ffi_create(&ffi, "GPG", "GPG") != RNP_SUCCESS) {
        return result;
    }

    /* load public keyring - we do not need secret for encryption */
    if (rnp_input_from_path(&keyfile, "pubring.pgp") != RNP_SUCCESS) {
        fprintf(stdout, "failed to open pubring.pgp. Did you run ./generate sample?\n");
        goto finish;
    }

    /* we may use RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_PUBLIC_KEYS as well */
    if (rnp_load_keys(ffi, "GPG", keyfile, RNP_LOAD_SAVE_PUBLIC_KEYS) != RNP_SUCCESS) {
        fprintf(stdout, "failed to read pubring.pgp\n");
        goto finish;
    }
    rnp_input_destroy(keyfile);
    keyfile = NULL;

    /* create memory input and file output objects for the message and encrypted message */
    if (rnp_input_from_memory(&input, (uint8_t *) message, strlen(message), false) !=
        RNP_SUCCESS) {
        fprintf(stdout, "failed to create input object\n");
        goto finish;
    }

    if (rnp_output_to_path(&output, "encrypted.asc") != RNP_SUCCESS) {
        fprintf(stdout, "failed to create output object\n");
        goto finish;
    }

    /* create encryption operation */
    if (rnp_op_encrypt_create(&encrypt, ffi, input, output) != RNP_SUCCESS) {
        fprintf(stdout, "failed to create encrypt operation\n");
        goto finish;
    }

    /* setup encryption parameters */
    rnp_op_encrypt_set_armor(encrypt, true);
    rnp_op_encrypt_set_file_name(encrypt, "message.txt");
    rnp_op_encrypt_set_file_mtime(encrypt, (uint32_t) time(NULL));
    rnp_op_encrypt_set_compression(encrypt, "ZIP", 6);
    rnp_op_encrypt_set_cipher(encrypt, RNP_ALGNAME_AES_256);
    rnp_op_encrypt_set_aead(encrypt, "None");

    /* locate recipient's key and add it to the operation context. While we search by userid
     * (which is easier), you can search by keyid, fingerprint or grip. */
    if (rnp_locate_key(ffi, "userid", "rsa@key", &key) != RNP_SUCCESS) {
        fprintf(stdout, "failed to locate recipient key rsa@key.\n");
        goto finish;
    }

    if (rnp_op_encrypt_add_recipient(encrypt, key) != RNP_SUCCESS) {
        fprintf(stdout, "failed to add recipient\n");
        goto finish;
    }
    rnp_key_handle_destroy(key);
    key = NULL;

    /* add encryption password as well */
    if (rnp_op_encrypt_add_password(
          encrypt, "encpassword", RNP_ALGNAME_SHA256, 0, RNP_ALGNAME_AES_256) != RNP_SUCCESS) {
        fprintf(stdout, "failed to add encryption password\n");
        goto finish;
    }

    /* execute encryption operation */
    if (rnp_op_encrypt_execute(encrypt) != RNP_SUCCESS) {
        fprintf(stdout, "encryption failed\n");
        goto finish;
    }

    fprintf(stdout, "Encryption succeeded. Encrypted message written to file encrypted.asc\n");
    result = 0;
finish:
    rnp_op_encrypt_destroy(encrypt);
    rnp_input_destroy(keyfile);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_key_handle_destroy(key);
    rnp_ffi_destroy(ffi);
    return result;
}

int
main(int argc, char **argv)
{
    return ffi_encrypt();
}
