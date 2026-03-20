/*
 * Copyright (c) 2017-2020 [Ribose Inc](https://www.ribose.com).
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

#include <fstream>
#include <vector>
#include <string>

#include <rnp/rnp.h>
#include "rnp_tests.h"
#include "support.h"
#include "utils.h"
#include <json.h>
#include <vector>
#include <string>

// this reader produces errors
static bool
error_reader(void *app_ctx, void *buf, size_t len, size_t *read)
{
    return false;
}

// this writer produces errors
static bool
error_writer(void *app_ctx, const void *buf, size_t len)
{
    return false;
}

static bool
ignoring_writer(void *app_ctx, const void *buf, size_t len)
{
    return true;
}

TEST_F(rnp_tests, test_pipe)
{
    uint8_t *         buf = NULL;
    size_t            buf_size = 0;
    rnp_input_t       input = NULL;
    rnp_output_t      output = NULL;
    const std::string msg("this is a test");

    assert_rnp_success(
      rnp_input_from_memory(&input, (const uint8_t *) msg.data(), msg.size(), true));
    assert_rnp_success(rnp_output_to_memory(&output, 0));

    assert_rnp_failure(rnp_output_pipe(input, NULL));
    assert_rnp_failure(rnp_output_pipe(NULL, output));
    assert_rnp_success(rnp_output_pipe(input, output));
    assert_rnp_success(rnp_output_finish(output));
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &buf_size, false));
    std::string data = std::string(buf, buf + buf_size);
    assert_string_equal(data.c_str(), msg.c_str());

    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
}

TEST_F(rnp_tests, test_pipe_source_error)
{
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    assert_rnp_success(rnp_input_from_callback(&input, error_reader, NULL, NULL));
    assert_rnp_success(rnp_output_to_null(&output));

    assert_rnp_failure(rnp_output_pipe(input, output));

    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
}

TEST_F(rnp_tests, test_pipe_dest_error)
{
    rnp_input_t       input = NULL;
    rnp_output_t      output = NULL;
    const std::string msg("this is a test");

    assert_rnp_success(
      rnp_input_from_memory(&input, (const uint8_t *) msg.data(), msg.size(), true));
    assert_rnp_success(rnp_output_to_callback(&output, error_writer, NULL, NULL));

    assert_rnp_failure(rnp_output_pipe(input, output));

    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
}

TEST_F(rnp_tests, test_output_write)
{
    rnp_output_t      output = NULL;
    const std::string msg("this is a test");

    assert_rnp_failure(rnp_output_to_callback(NULL, ignoring_writer, NULL, NULL));
    assert_rnp_failure(rnp_output_to_callback(&output, NULL, NULL, NULL));
    assert_rnp_success(rnp_output_to_callback(&output, ignoring_writer, NULL, NULL));
    size_t written = 100;
    assert_rnp_failure(rnp_output_write(NULL, msg.c_str(), msg.size(), &written));
    assert_rnp_failure(rnp_output_write(output, NULL, 10, &written));
    assert_rnp_success(rnp_output_write(output, NULL, 0, &written));
    assert_int_equal(written, 0);
    assert_rnp_success(rnp_output_write(output, msg.c_str(), msg.size(), NULL));
    assert_rnp_failure(rnp_output_finish(NULL));
    assert_rnp_success(rnp_output_destroy(output));
}
