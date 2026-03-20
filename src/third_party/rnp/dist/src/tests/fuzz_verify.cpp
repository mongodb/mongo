/*
 * Copyright (c) 2021, [Ribose Inc](https://www.ribose.com).
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
#include <librepgp/stream-write.h>

extern "C" int verify_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define DATA_PATH "data/test_fuzz_verify/"

TEST_F(rnp_tests, test_fuzz_verify)
{
    auto data = file_to_vec(DATA_PATH "timeout-25b8c9d824c8eb492c827689795748298a2b0a46");
    assert_int_equal(verify_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "timeout-c2aff538c73b447bca689005e9762840b5a022d0");
    assert_int_equal(verify_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "timeout-5229070269153280");
    assert_int_equal(verify_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "timeout-6613852539453440");
    assert_int_equal(verify_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
}
