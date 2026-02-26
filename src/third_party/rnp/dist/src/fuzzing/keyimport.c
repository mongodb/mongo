/*
 * Copyright (c) 2020, [Ribose Inc](https://www.ribose.com).
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
#include <rnp/rnp_err.h>

#ifdef RNP_RUN_TESTS
int keyimport_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int
keyimport_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
#else
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
#endif
{
    rnp_input_t  input = NULL;
    rnp_result_t ret = 0;
    rnp_ffi_t    ffi = NULL;
    uint32_t     flags = RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS;

    /* try non-permissive import */
    ret = rnp_input_from_memory(&input, data, size, false);
    ret = rnp_ffi_create(&ffi, "GPG", "GPG");
    char *results = NULL;
    ret = rnp_import_keys(ffi, input, flags, &results);
    rnp_buffer_destroy(results);
    rnp_input_destroy(input);
    rnp_ffi_destroy(ffi);

    /* try permissive import */
    rnp_input_t input2 = NULL;
    ret = rnp_input_from_memory(&input2, data, size, false);
    ret = rnp_ffi_create(&ffi, "GPG", "GPG");
    results = NULL;
    ret = rnp_import_keys(ffi, input2, flags | RNP_LOAD_SAVE_PERMISSIVE, &results);
    rnp_buffer_destroy(results);
    rnp_input_destroy(input2);
    rnp_ffi_destroy(ffi);

    /* try non-permissive iterative import */
    rnp_input_t input3 = NULL;
    ret = rnp_input_from_memory(&input3, data, size, false);
    ret = rnp_ffi_create(&ffi, "GPG", "GPG");
    flags |= RNP_LOAD_SAVE_SINGLE;
    do {
        results = NULL;
        ret = rnp_import_keys(ffi, input3, flags, &results);
        rnp_buffer_destroy(results);
    } while (!ret);
    rnp_input_destroy(input3);
    rnp_ffi_destroy(ffi);

    /* try permissive iterative import */
    rnp_input_t input4 = NULL;
    ret = rnp_input_from_memory(&input4, data, size, false);
    ret = rnp_ffi_create(&ffi, "GPG", "GPG");
    flags |= RNP_LOAD_SAVE_PERMISSIVE;
    do {
        results = NULL;
        ret = rnp_import_keys(ffi, input4, flags, &results);
        rnp_buffer_destroy(results);
    } while (!ret);
    rnp_input_destroy(input4);
    rnp_ffi_destroy(ffi);

    return 0;
}
