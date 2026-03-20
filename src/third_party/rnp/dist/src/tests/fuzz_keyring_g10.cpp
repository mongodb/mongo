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

int keyring_g10_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define DATA_PATH "data/test_fuzz_keyring_g10/"

TEST_F(rnp_tests, test_fuzz_keyring_g10)
{
    auto data = file_to_vec(DATA_PATH "crash_c9cabce6f8d7b36fde0306c86ce81c4f554cbd2a");
    assert_int_equal(keyring_g10_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "crash_4ec166859e821aee27350dcde3e9c06b07a677f7");
    assert_int_equal(keyring_g10_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "crash_5528625325932544");
    assert_int_equal(keyring_g10_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
}
