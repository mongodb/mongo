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

#define RNP_SUCCESS 0

/* sample pass provider implementation, which always return 'password' for key decryption and
 * 'encpassword' when password is needed for file decryption. You may ask for password via
 * stdin, or choose password based on key properties, whatever else */
static bool
example_pass_provider(rnp_ffi_t        ffi,
                      void *           app_ctx,
                      rnp_key_handle_t key,
                      const char *     pgp_context,
                      char             buf[],
                      size_t           buf_len)
{
    if (!strcmp(pgp_context, "decrypt (symmetric)")) {
        strncpy(buf, "encpassword", buf_len);
        return true;
    }
    if (!strcmp(pgp_context, "decrypt")) {
        strncpy(buf, "password", buf_len);
        return true;
    }

    return false;
}

static int
ffi_decrypt(bool usekeys)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  keyfile = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;
    uint8_t *    buf = NULL;
    size_t       buf_len = 0;
    int          result = 1;

    /* initialize FFI object */
    if (rnp_ffi_create(&ffi, "GPG", "GPG") != RNP_SUCCESS) {
        return result;
    }

    /* check whether we want to use key or password for decryption */
    if (usekeys) {
        /* load secret keyring, as it is required for public-key decryption. However, you may
         * need to load public keyring as well to validate key's signatures. */
        if (rnp_input_from_path(&keyfile, "secring.pgp") != RNP_SUCCESS) {
            fprintf(stdout, "failed to open secring.pgp. Did you run ./generate sample?\n");
            goto finish;
        }

        /* we may use RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_PUBLIC_KEYS as well*/
        if (rnp_load_keys(ffi, "GPG", keyfile, RNP_LOAD_SAVE_SECRET_KEYS) != RNP_SUCCESS) {
            fprintf(stdout, "failed to read secring.pgp\n");
            goto finish;
        }
        rnp_input_destroy(keyfile);
        keyfile = NULL;
    }

    /* set the password provider */
    rnp_ffi_set_pass_provider(ffi, example_pass_provider, NULL);

    /* create file input and memory output objects for the encrypted message and decrypted
     * message */
    if (rnp_input_from_path(&input, "encrypted.asc") != RNP_SUCCESS) {
        fprintf(stdout, "failed to create input object\n");
        goto finish;
    }

    if (rnp_output_to_memory(&output, 0) != RNP_SUCCESS) {
        fprintf(stdout, "failed to create output object\n");
        goto finish;
    }

    if (rnp_decrypt(ffi, input, output) != RNP_SUCCESS) {
        fprintf(stdout, "public-key decryption failed\n");
        goto finish;
    }

    /* get the decrypted message from the output structure */
    if (rnp_output_memory_get_buf(output, &buf, &buf_len, false) != RNP_SUCCESS) {
        goto finish;
    }
    fprintf(stdout,
            "Decrypted message (%s):\n%.*s\n",
            usekeys ? "with key" : "with password",
            (int) buf_len,
            buf);

    result = 0;
finish:
    rnp_input_destroy(keyfile);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_ffi_destroy(ffi);
    return result;
}

int
main(int argc, char **argv)
{
    int res;
    res = ffi_decrypt(true);
    if (res) {
        return res;
    }
    res = ffi_decrypt(false);
    return res;
}