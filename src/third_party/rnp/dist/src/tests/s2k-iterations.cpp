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

#include <rnp/rnp.h>
#include "rnp_tests.h"
#include "support.h"
#include "utils.h"
#include "librepgp/stream-common.h"
#include "librepgp/stream-packet.h"

static void
test_s2k_iterations_value(rnp_ffi_t ffi,
                          size_t    input_iterations,
                          size_t    expected_iterations,
                          bool      exact_match)
{
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_encrypt_t op = NULL;
    const char *     message = "RNP encryption sample message";
    assert_rnp_success(
      rnp_input_from_memory(&input, (uint8_t *) message, strlen(message), false));
    assert_rnp_success(rnp_output_to_memory(&output, 1024));
    // create encrypt operation
    assert_rnp_success(rnp_op_encrypt_create(&op, ffi, input, output));
    // add password and specify iterations for key derivation here
    assert_rnp_success(rnp_op_encrypt_add_password(op, "pass1", NULL, input_iterations, NULL));
    assert_rnp_success(rnp_op_encrypt_execute(op));
    // testing the saved packets
    {
        rnp_input_t input_dump = NULL;
        char *      json = NULL;
        uint8_t *   mem = NULL;
        size_t      len = 0;
        // get the output and dump it to JSON
        assert_rnp_success(rnp_output_memory_get_buf(output, &mem, &len, false));
        assert_rnp_success(rnp_input_from_memory(&input_dump, mem, len, false));
        assert_rnp_success(rnp_dump_packets_to_json(input_dump, 0, &json));
        assert_non_null(json);
        json_object *jso = json_tokener_parse(json);
        rnp_buffer_destroy(json);
        assert_non_null(jso);
        assert_true(json_object_is_type(jso, json_type_array));
        // check the symmetric-key encrypted session key packet
        json_object *pkt = json_object_array_get_idx(jso, 0);
        assert_true(check_json_pkt_type(pkt, PGP_PKT_SK_SESSION_KEY));
        json_object *s2k = NULL;
        assert_true(json_object_object_get_ex(pkt, "s2k", &s2k));
        json_object *fld = NULL;
        assert_true(json_object_object_get_ex(s2k, "iterations", &fld));
        assert_true(json_object_is_type(fld, json_type_int));
        // there was already decoded value in JSON
        size_t extracted_value = (size_t) json_object_get_int(fld);
        if (exact_match) {
            assert_int_equal(extracted_value, expected_iterations);
        } else {
            assert_true(extracted_value >= expected_iterations);
        }
        // cleanup
        json_object_put(jso); // release the JSON object
        assert_rnp_success(rnp_input_destroy(input_dump));
    }
    assert_rnp_success(rnp_op_encrypt_destroy(op));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_input_destroy(input));
}

TEST_F(rnp_tests, test_s2k_iterations)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    test_s2k_iterations_value(ffi, 0, 1024, false);
    test_s2k_iterations_value(ffi, 1024, 1024, true);
    test_s2k_iterations_value(ffi, 1088, 1088, true);
    const size_t MAX_ITER = 0x3e00000; // 0x1F << (0xF + 6);
    test_s2k_iterations_value(ffi, MAX_ITER - 1, MAX_ITER, true);
    test_s2k_iterations_value(ffi, MAX_ITER, MAX_ITER, true);
    test_s2k_iterations_value(ffi, MAX_ITER + 1, MAX_ITER, true);
    test_s2k_iterations_value(ffi, SIZE_MAX, MAX_ITER, true);
    assert_rnp_success(rnp_ffi_destroy(ffi));
}
