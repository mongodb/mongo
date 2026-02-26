/*
 * Copyright (c) 2017-2019 [Ribose Inc](https://www.ribose.com).
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
#include <librekey/kbx_blob.hpp>
#include "rnp_tests.h"

#define BLOB_HEADER_SIZE 0x5
#define BLOB_FIRST_SIZE 0x20

static uint8_t
ru8(uint8_t *p)
{
    return (uint8_t) p[0];
}

static uint32_t
ru32(uint8_t *p)
{
    return (uint32_t)(((uint8_t) p[0] << 24) | ((uint8_t) p[1] << 16) | ((uint8_t) p[2] << 8) |
                      (uint8_t) p[3]);
}

// This is rnp_key_store_kbx_parse_header_blob() adjusted for test
static void
test_parse_header_blob(kbx_header_blob_t &first_blob)
{
    assert_int_equal(first_blob.length(), BLOB_FIRST_SIZE);
    assert_true(first_blob.parse());
}

// This is rnp_key_store_kbx_parse_pgp_blob() adjusted for test
static void
test_parse_pgp_blob(kbx_pgp_blob_t &pgp_blob)
{
    assert_true(pgp_blob.parse());
    assert_false(pgp_blob.keyblock_offset() > pgp_blob.length() ||
                 pgp_blob.length() <
                   (pgp_blob.keyblock_offset() + pgp_blob.keyblock_length()));
}

// This test ensures that NSIGS field of keybox PGP blob contains the total number of
// signatures, including subkey's
TEST_F(rnp_tests, test_kbx_nsigs)
{
    rnp_ffi_t ffi = NULL;
    size_t    pubring_bufsize = 4096; // buffer size, large enough to hold public keyring
    // init ffi
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    // 1. Generate key and subkey
    // generate RSA key
    rnp_op_generate_t keygen = NULL;
    assert_rnp_success(rnp_op_generate_create(&keygen, ffi, "RSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1024));
    // user id
    assert_rnp_success(rnp_op_generate_set_userid(keygen, "userid"));
    // now execute keygen operation
    assert_rnp_success(rnp_op_generate_execute(keygen));
    rnp_key_handle_t key = NULL;
    assert_rnp_success(rnp_op_generate_get_key(keygen, &key));
    assert_non_null(key);
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    keygen = NULL;
    // generate DSA subkey
    assert_rnp_success(rnp_op_generate_subkey_create(&keygen, ffi, key, "DSA"));
    assert_rnp_success(rnp_op_generate_set_bits(keygen, 1536));
    assert_rnp_success(rnp_op_generate_set_dsa_qbits(keygen, 224));
    // now generate the subkey
    assert_rnp_success(rnp_op_generate_execute(keygen));
    assert_rnp_success(rnp_op_generate_destroy(keygen));
    keygen = NULL;
    assert_rnp_success(rnp_key_handle_destroy(key));
    key = NULL;
    // 2. Save the public keys to memory
    rnp_output_t output = NULL;
    assert_rnp_success(rnp_output_to_memory(&output, pubring_bufsize));
    assert_rnp_success(
      rnp_save_keys(ffi, RNP_KEYSTORE_KBX, output, RNP_LOAD_SAVE_PUBLIC_KEYS));
    // 3. Read and test the keybox blobs
    uint8_t *buf = NULL;
    size_t   has_bytes = 0;
    assert_rnp_success(rnp_output_memory_get_buf(output, &buf, &has_bytes, false));
    { // header blob
        assert_true(has_bytes >= BLOB_HEADER_SIZE);
        uint32_t blob_length = ru32(buf);
        assert_true(has_bytes >= blob_length);
        kbx_blob_type_t type = (kbx_blob_type_t) ru8(buf + 4);
        assert_int_equal(type, KBX_HEADER_BLOB);
        std::vector<uint8_t> data(buf, buf + blob_length);
        kbx_header_blob_t    header_blob(data);
        test_parse_header_blob(header_blob);
        has_bytes -= blob_length;
        buf += blob_length;
    }
    { // PGP blob
        assert_true(has_bytes >= BLOB_HEADER_SIZE);
        uint32_t blob_length = ru32(buf);
        assert_true(has_bytes >= blob_length);
        kbx_blob_type_t type = (kbx_blob_type_t) ru8(buf + 4);
        assert_int_equal(type, KBX_PGP_BLOB);
        std::vector<uint8_t> data(buf, buf + blob_length);
        kbx_pgp_blob_t       pgp_blob(data);
        test_parse_pgp_blob(pgp_blob);
        assert_int_equal(pgp_blob.nkeys(), 2); // key and subkey
        assert_int_equal(pgp_blob.nsigs(), 2); // key and subkey signatures
        has_bytes -= blob_length;
        buf += blob_length;
    }
    assert_int_equal(has_bytes, 0); // end of keybox
    // cleanup
    assert_rnp_success(rnp_output_destroy(output));
    assert_rnp_success(rnp_ffi_destroy(ffi));
}
