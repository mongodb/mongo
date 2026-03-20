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

/* example key provider which loads key from file based on its keyid */
static void
example_key_provider(rnp_ffi_t   ffi,
                     void *      app_ctx,
                     const char *identifier_type,
                     const char *identifier,
                     bool        secret)
{
    rnp_input_t input = NULL;
    char        filename[32] = {0};
    if (strcmp(identifier_type, "keyid")) {
        if (strcmp(identifier_type, "fingerprint")) {
            fprintf(stdout, "Unsupported key search: %s = %s\n", identifier_type, identifier);
            return;
        }
        /* if we search by fp then keyid is last 16 chars */
        if (strlen(identifier) < 40) {
            fprintf(stdout, "Invalid fingerprint: %s\n", identifier);
            return;
        }
        identifier += 24;
    }

    snprintf(filename, sizeof(filename), "key-%s-%s.asc", identifier, secret ? "sec" : "pub");

    if (rnp_input_from_path(&input, filename) != RNP_SUCCESS) {
        fprintf(stdout, "failed to open key file %s\n", filename);
        return;
    }

    if (rnp_load_keys(
          ffi, "GPG", input, RNP_LOAD_SAVE_SECRET_KEYS | RNP_LOAD_SAVE_PUBLIC_KEYS) !=
        RNP_SUCCESS) {
        fprintf(stdout, "failed to load key from file %s\n", filename);
    }
    rnp_input_destroy(input);
}

static int
ffi_verify()
{
    rnp_ffi_t       ffi = NULL;
    rnp_op_verify_t verify = NULL;
    rnp_input_t     input = NULL;
    rnp_output_t    output = NULL;
    uint8_t *       buf = NULL;
    size_t          buf_len = 0;
    size_t          sigcount = 0;
    int             result = 1;

    /* initialize FFI object */
    if (rnp_ffi_create(&ffi, "GPG", "GPG") != RNP_SUCCESS) {
        return result;
    }

    /* we do not load any keys here since we'll use key provider */
    rnp_ffi_set_key_provider(ffi, example_key_provider, NULL);

    /* create file input and memory output objects for the signed message and verified
     * message */
    if (rnp_input_from_path(&input, "signed.asc") != RNP_SUCCESS) {
        fprintf(stdout, "failed to open file 'signed.asc'. Did you run the sign example?\n");
        goto finish;
    }

    if (rnp_output_to_memory(&output, 0) != RNP_SUCCESS) {
        fprintf(stdout, "failed to create output object\n");
        goto finish;
    }

    if (rnp_op_verify_create(&verify, ffi, input, output) != RNP_SUCCESS) {
        fprintf(stdout, "failed to create verification context\n");
        goto finish;
    }

    if (rnp_op_verify_execute(verify) != RNP_SUCCESS) {
        fprintf(stdout, "failed to execute verification operation\n");
        goto finish;
    }

    /* now check signatures and get some info about them */
    if (rnp_op_verify_get_signature_count(verify, &sigcount) != RNP_SUCCESS) {
        fprintf(stdout, "failed to get signature count\n");
        goto finish;
    }

    for (size_t i = 0; i < sigcount; i++) {
        rnp_op_verify_signature_t sig = NULL;
        rnp_result_t              sigstatus = RNP_SUCCESS;
        rnp_key_handle_t          key = NULL;
        char *                    keyid = NULL;

        if (rnp_op_verify_get_signature_at(verify, i, &sig) != RNP_SUCCESS) {
            fprintf(stdout, "failed to get signature %d\n", (int) i);
            goto finish;
        }

        if (rnp_op_verify_signature_get_key(sig, &key) != RNP_SUCCESS) {
            fprintf(stdout, "failed to get signature's %d key\n", (int) i);
            goto finish;
        }

        if (rnp_key_get_keyid(key, &keyid) != RNP_SUCCESS) {
            fprintf(stdout, "failed to get key id %d\n", (int) i);
            rnp_key_handle_destroy(key);
            goto finish;
        }

        sigstatus = rnp_op_verify_signature_get_status(sig);
        fprintf(stdout, "Status for signature from key %s : %d\n", keyid, (int) sigstatus);
        rnp_buffer_destroy(keyid);
        rnp_key_handle_destroy(key);
    }

    /* get the verified message from the output structure */
    if (rnp_output_memory_get_buf(output, &buf, &buf_len, false) != RNP_SUCCESS) {
        goto finish;
    }
    fprintf(stdout, "Verified message:\n%.*s\n", (int) buf_len, buf);

    result = 0;
finish:
    rnp_op_verify_destroy(verify);
    rnp_input_destroy(input);
    rnp_output_destroy(output);
    rnp_ffi_destroy(ffi);
    return result;
}

int
main(int argc, char **argv)
{
    return ffi_verify();
}
