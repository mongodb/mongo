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

extern "C" int dump_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define DATA_PATH "data/test_fuzz_dump/"

TEST_F(rnp_tests, test_fuzz_dump)
{
    auto data =
      file_to_vec(DATA_PATH "clusterfuzz-testcase-minimized-fuzz_dump-5757362284265472");
    time_t start = time(NULL);
    assert_int_equal(dump_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
    assert_true(time(NULL) - start <= 1800);

    data = file_to_vec(DATA_PATH "timeout-7e498daecad7ee646371a466d4a317c59fe7db89");
    start = time(NULL);
    assert_int_equal(dump_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
    assert_true(time(NULL) - start <= 30);

    data = file_to_vec(DATA_PATH "timeout-6462239459115008");
    start = time(NULL);
    assert_int_equal(dump_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
    assert_true(time(NULL) - start <= 30);

    data = file_to_vec(DATA_PATH "outofmemory-5570076898623488");
    assert_int_equal(dump_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "timeout-6057122298462208");
    assert_int_equal(dump_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "outofmemory-6111789935624192");
    assert_int_equal(dump_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "abrt-5093675862917120");
    assert_int_equal(dump_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
}
