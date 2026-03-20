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
#include <librepgp/stream-write.h>

extern "C" int verify_detached_LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define DATA_PATH "data/test_fuzz_verify_detached/"

TEST_F(rnp_tests, test_fuzz_verify_detached)
{
    auto data = file_to_vec(
      DATA_PATH "clusterfuzz-testcase-minimized-fuzz_verify_detached-5092660526972928");
    assert_int_equal(verify_detached_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "outofmemory-23094cb781b2cf6d1749ebac8bd0576e51440498-z");
    assert_int_equal(verify_detached_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "outofmemory-dea88a4aa4ab5fec1291446db702ee893d5559cf");
    assert_int_equal(verify_detached_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);

    data = file_to_vec(DATA_PATH "invalid-enum-value-4717481657171968");
    assert_int_equal(verify_detached_LLVMFuzzerTestOneInput(data.data(), data.size()), 0);
}
