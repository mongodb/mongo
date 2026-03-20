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

extern "C" int keyimport_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define DATA_PATH "data/test_fuzz_keyimport/"

TEST_F(rnp_tests, test_fuzz_keyimport)
{
    auto data = file_to_vec(DATA_PATH "crash_25f06f13b48d58a5faf6c36fae7fcbd958359199");
    assert_int_equal(keyimport_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "crash_e932261875271ccf497715de56adf7caf30ca8a7");
    assert_int_equal(keyimport_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "crash_37e8ed57ee47c1991b387fa0506f361f9cd9c663");
    assert_int_equal(keyimport_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "leak_11307b70cc609c93fc3a49d37f3a31166df50f44");
    assert_int_equal(keyimport_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "timeout_9c10372fe9ebdcdb0b6e275d05f8af4f4e3d6051");
    assert_int_equal(keyimport_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "leak_371b211d7e9cf9857befcf06c7da74835e249ee7");
    assert_int_equal(keyimport_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "timeout-9c10372fe9ebdcdb0b6e275d05f8af4f4e3d6051");
    assert_int_equal(keyimport_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
}
