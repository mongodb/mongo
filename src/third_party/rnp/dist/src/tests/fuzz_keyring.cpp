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

extern "C" int keyring_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define DATA_PATH "data/test_fuzz_keyring/"

TEST_F(rnp_tests, test_fuzz_keyring)
{
    auto data = file_to_vec(DATA_PATH "leak-542d4e51506e3e9d34c9b243e608a964dabfdb21");
    assert_int_equal(data.size(), 540);
    assert_int_equal(keyring_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "crash-7ff10f10a95b78461d6f3578f5f99e870c792b9f");
    assert_int_equal(keyring_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    /* Issue 25292 in oss-fuzz: rnp:fuzz_keyring: Stack-buffer-overflow in stream_write_key */
    data = file_to_vec(DATA_PATH "crash-8619144979e56d07ab4890bf564b90271ae9b1c9");
    assert_int_equal(keyring_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    /* Issue 25302 in oss-fuzz: rnp:fuzz_keyring: Direct-leak in Botan::HashFunction::create */
    data = file_to_vec(DATA_PATH "leak-5ee77f7ae99d7815d069afe037c42f4887193215");
    assert_int_equal(keyring_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "timeout-6140201111519232");
    assert_int_equal(keyring_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
}
