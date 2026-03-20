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
#include "rnp_tests.h"
#include "support.h"

extern "C" int keyring_kbx_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define DATA_PATH "data/test_fuzz_keyring_kbx/"

TEST_F(rnp_tests, test_fuzz_keyring_kbx)
{
    auto data = file_to_vec(DATA_PATH "leak-52c65c00b53997178f4cd9defa0343573ea8dda6");
    assert_int_equal(keyring_kbx_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    /* Issue 25386 in oss-fuzz: rnp:fuzz_keyring_kbx: Heap-buffer-overflow in
     * rnp_key_store_kbx_from_src */
    data = file_to_vec(DATA_PATH "crash-5526a2e13255018c857ce493c28ce7108b8b2987");
    assert_int_equal(keyring_kbx_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    /* Issue 25388 in oss-fuzz: rnp:fuzz_keyring_kbx: Heap-buffer-overflow in mem_src_read */
    data = file_to_vec(DATA_PATH "crash-b894a2f79f7d38a16ae0ee8d74972336aa3f5798");
    assert_int_equal(keyring_kbx_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    /* Leak found during CI run */
    data = file_to_vec(DATA_PATH "leak-b02cd1c6b70c10a8a673a34ba3770b39468b7ddf");
    assert_int_equal(keyring_kbx_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    /* Issue 68648 in oss-fuzz: rnp:fuzz_keyring_kbx: Timeout in fuzz_keyring_kbx */
    data = file_to_vec(DATA_PATH "timeout-5101021410951168");
    assert_int_equal(keyring_kbx_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
}
