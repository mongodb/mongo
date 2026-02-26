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
#include "librepgp/stream-common.h"
#include "librepgp/stream-packet.h"
#include "utils.h"
#include <json.h>
#include <vector>
#include <string>

// structure for filling input
typedef struct {
    uint32_t remaining;
    uint8_t  dummy;
} dummy_reader_ctx_st;

// reader of sequence of dummy bytes
static bool
dummy_reader(void *app_ctx, void *buf, size_t len, size_t *read)
{
    size_t               filled = 0;
    dummy_reader_ctx_st *ctx = NULL;
    ctx = (dummy_reader_ctx_st *) app_ctx;
    filled = (len > ctx->remaining) ? ctx->remaining : len;
    if (filled > 0) {
        memset(buf, ctx->dummy, filled);
        ctx->remaining -= filled;
    }
    *read = filled;
    return true;
}

static void
test_partial_length_init(rnp_ffi_t *ffi, uint32_t key_flags)
{
    rnp_input_t input = NULL;
    // init ffi
    assert_rnp_success(rnp_ffi_create(ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_ffi_set_pass_provider(*ffi, ffi_string_password_provider, (void *) "password"));
    if (key_flags & RNP_LOAD_SAVE_SECRET_KEYS) {
        assert_rnp_success(rnp_input_from_path(&input, "data/keyrings/1/secring.gpg"));
        assert_rnp_success(rnp_load_keys(*ffi, "GPG", input, key_flags));
        assert_rnp_success(rnp_input_destroy(input));
    }
    if (key_flags & RNP_LOAD_SAVE_PUBLIC_KEYS) {
        assert_rnp_success(rnp_input_from_path(&input, "data/keyrings/1/pubring.gpg"));
        assert_rnp_success(rnp_load_keys(*ffi, "GPG", input, key_flags));
        assert_rnp_success(rnp_input_destroy(input));
    }
}

TEST_F(rnp_tests, test_partial_length_public_key)
{
    rnp_input_t input = NULL;
    rnp_ffi_t   ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_partial_length/pubring.gpg.partial"));
    assert_int_equal(rnp_load_keys(ffi, "GPG", input, RNP_LOAD_SAVE_PUBLIC_KEYS),
                     RNP_ERROR_BAD_FORMAT);
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_partial_length_signature)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;
    // init ffi
    test_partial_length_init(&ffi, RNP_LOAD_SAVE_PUBLIC_KEYS);
    // message having partial length signature packet
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_partial_length/message.txt.partial-signed"));
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    // cleanup
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_partial_length_first_packet_256)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;
    // init ffi
    test_partial_length_init(&ffi, RNP_LOAD_SAVE_PUBLIC_KEYS);
    // message having first partial length packet of 256 bytes
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_partial_length/message.txt.partial-256"));
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    // cleanup
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_partial_length_zero_last_chunk)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;
    // init ffi
    test_partial_length_init(&ffi, RNP_LOAD_SAVE_PUBLIC_KEYS);
    // message in partial packets having 0-size last chunk
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_partial_length/message.txt.partial-zero-last"));
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    // cleanup
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_partial_length_largest)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;
    // init ffi
    test_partial_length_init(&ffi, RNP_LOAD_SAVE_PUBLIC_KEYS);
    // message having largest possible partial packet
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_partial_length/message.txt.partial-1g"));
    assert_rnp_success(rnp_output_to_null(&output));
    rnp_op_verify_t verify = NULL;
    assert_rnp_success(rnp_op_verify_create(&verify, ffi, input, output));
    assert_rnp_success(rnp_op_verify_execute(verify));
    // cleanup
    assert_rnp_success(rnp_op_verify_destroy(verify));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}

TEST_F(rnp_tests, test_partial_length_first_packet_length)
{
    rnp_ffi_t        ffi = NULL;
    rnp_input_t      input = NULL;
    rnp_output_t     output = NULL;
    rnp_op_sign_t    sign = NULL;
    rnp_key_handle_t key = NULL;
    // uncacheable size will emulate unknown length from callback source
    size_t uncacheable_size = PGP_INPUT_CACHE_SIZE + 1;
    // init ffi
    test_partial_length_init(&ffi, RNP_LOAD_SAVE_SECRET_KEYS);
    // generate a sequence of octets with appropriate length using callback
    dummy_reader_ctx_st reader_ctx;
    reader_ctx.dummy = 'X';
    reader_ctx.remaining = uncacheable_size;
    assert_rnp_success(rnp_input_from_callback(&input, dummy_reader, NULL, &reader_ctx));
    assert_rnp_success(rnp_output_to_memory(&output, uncacheable_size + 1024));
    assert_rnp_success(rnp_op_sign_create(&sign, ffi, input, output));
    assert_rnp_success(rnp_locate_key(ffi, "keyid", "7BC6709B15C23A4A", &key));
    assert_rnp_success(rnp_op_sign_add_signature(sign, key, NULL));
    assert_rnp_success(rnp_key_handle_destroy(key));
    key = NULL;
    // signing
    assert_rnp_success(rnp_op_sign_execute(sign));
    // read from the saved packets
    pgp_source_t src;
    uint8_t *    mem = NULL;
    size_t       len = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &mem, &len, false));
    assert_rnp_success(init_mem_src(&src, mem, len, false));
    // skip first packet (one-pass signature)
    pgp_packet_body_t body(PGP_PKT_ONE_PASS_SIG);
    assert_rnp_success(body.read(src));
    // checking next packet header (should be partial length literal data)
    uint8_t flags = 0;
    assert_true(src.read_eq(&flags, 1));
    assert_int_equal(flags, PGP_PTAG_ALWAYS_SET | PGP_PTAG_NEW_FORMAT | PGP_PKT_LITDATA);
    // checking length
    bool last = true; // should be reset by stream_read_partial_chunk_len()
    assert_true(stream_read_partial_chunk_len(&src, &len, &last));
    assert_true(len >= PGP_PARTIAL_PKT_FIRST_PART_MIN_SIZE);
    assert_false(last);
    // cleanup
    src.close();
    assert_rnp_success(rnp_op_sign_destroy(sign));
    assert_rnp_success(rnp_input_destroy(input));
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}
