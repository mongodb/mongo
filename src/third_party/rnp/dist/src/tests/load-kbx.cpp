/*
 * Copyright (c) 2017-2023 [Ribose Inc](https://www.ribose.com).
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

#include "key.hpp"

#include "rnp_tests.h"
#include "support.h"

#include <string.h>

#define BLOB_HEADER_SIZE 0x5
#define BLOB_SIZE_LIMIT (5 * 1024 * 1024)
#define BLOB_FIRST_SIZE 0x20
#define BLOB_KEY_SIZE 0x1C
#define BLOB_UID_SIZE 0x0C
#define BLOB_SIG_SIZE 0x04

static void
w32(uint8_t *dst, uint32_t val)
{
    dst[0] = val >> 24;
    dst[1] = val >> 16;
    dst[2] = val >> 8;
    dst[3] = val & 0xFF;
}

static void
make_valid_header_blob(uint8_t *buf)
{
    memset(buf, 0, BLOB_FIRST_SIZE);
    /* Length of this blob */
    w32(buf, BLOB_FIRST_SIZE);
    /* Blob type */
    buf[4] = KBX_HEADER_BLOB;
    /* Version */
    buf[5] = 1;
    /* Flags */
    buf[6] = 0;
    buf[7] = 0;
    memcpy(buf + 8, "KBXf", 4);
}

/* See kbx/keybox-blob.c in the gnupg sources for the KBX format spec. */
TEST_F(rnp_tests, test_invalid_kbx)
{
    rnp_ffi_t   ffi = NULL;
    rnp_input_t mem_input = NULL;
    uint8_t     buf[1024];

    memset(buf, 0, sizeof buf);
    assert_rnp_success(rnp_ffi_create(&ffi, "KBX", "GPG"));
    /* Available blob bytes < first blob min size  */
    w32(buf, BLOB_FIRST_SIZE);
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE - 1, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Stored blob size < BLOB_HEADER_SIZE */
    w32(buf, BLOB_HEADER_SIZE - 1);
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Stored blob size exceeds BLOB_SIZE_LIMIT */
    w32(buf, BLOB_SIZE_LIMIT + 1);
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Stored blob size > available blob bytes */
    w32(buf, BLOB_SIZE_LIMIT - 1);
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Unsupported blob type */
    w32(buf, BLOB_FIRST_SIZE);
    buf[4] = 0xFF;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* The first blob has wrong length */
    w32(buf, BLOB_FIRST_SIZE + 1);
    buf[4] = KBX_HEADER_BLOB;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 1, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Wrong version */
    w32(buf, BLOB_FIRST_SIZE);
    buf[4] = KBX_HEADER_BLOB;
    buf[5] = 0xFF;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* The first blob hasn't got a KBXf magic string */
    w32(buf, BLOB_FIRST_SIZE);
    buf[4] = KBX_HEADER_BLOB;
    buf[5] = 1;
    /* Flags */
    buf[6] = 0;
    buf[7] = 0;
    buf[8] = 0xFF;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Valid header blob + empty blob + excess trailing bytes */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, BLOB_HEADER_SIZE);
    buf[BLOB_FIRST_SIZE + 4] = KBX_EMPTY_BLOB;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + BLOB_HEADER_SIZE + 3, false));
    assert_rnp_success(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Valid header blob + too small PGP blob */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 19);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 19, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Valid header blob + wrong version in the PGP blob */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 0xFF;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Wrong keyblock offset in the PGP blob */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    /* Flags */
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    /* Keyblock offset */
    w32(buf + BLOB_FIRST_SIZE + 8, UINT32_MAX);
    /* Keyblock length */
    w32(buf + BLOB_FIRST_SIZE + 12, UINT32_MAX);
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* 0 keys in the PGP blob */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    /* Flags */
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    /* Keyblock offset */
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    /* Keyblock length */
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    /* NKEYS MSB */
    buf[BLOB_FIRST_SIZE + 16] = 0;
    /* NKEYS LSB */
    buf[BLOB_FIRST_SIZE + 17] = 0;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Too many keys in the PGP blob */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    /* Flags */
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    /* Keyblock offset */
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    /* Keyblock length */
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    /* NKEYS MSB */
    buf[BLOB_FIRST_SIZE + 16] = 0x80;
    /* NKEYS LSB */
    buf[BLOB_FIRST_SIZE + 17] = 1;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* Size of the key information structure < 28 */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    /* Flags */
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    /* Keyblock offset */
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    /* Keyblock length */
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    /* NKEYS MSB */
    buf[BLOB_FIRST_SIZE + 16] = 0;
    /* NKEYS LSB */
    buf[BLOB_FIRST_SIZE + 17] = 1;
    /* Size of the key information structure MSB */
    buf[BLOB_FIRST_SIZE + 18] = 0;
    /* Size of the key information structure LSB */
    buf[BLOB_FIRST_SIZE + 19] = BLOB_KEY_SIZE - 1;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too few bytes left for key blob" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    /* Size of the key information structure MSB */
    buf[BLOB_FIRST_SIZE + 18] = 0;
    /* Size of the key information structure LSB */
    buf[BLOB_FIRST_SIZE + 19] = BLOB_KEY_SIZE;
    assert_rnp_success(rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "No data for sn_size" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + 28);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = BLOB_KEY_SIZE;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* SN size exceeds available bytes */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    /* Size of the serial number MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0xFF;
    /* Size of the serial number LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0xFF;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too few data for uids" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too many uids in the PGP blob" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2 + 4);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    /* Number of user IDs MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 2] = 0x80;
    /* Number of user IDs LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 3] = 1;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2 + 4, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too few bytes for uid struct" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2 + 4);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    /* Number of user IDs MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 2] = 0;
    /* Number of user IDs LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 3] = 1;
    /* Size of user ID information structure MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 4] = 0;
    /* Size of user ID information structure LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 5] = BLOB_UID_SIZE - 1;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2 + 4, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too few bytes to read uid struct." */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2 + 4);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 2] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 3] = 1;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 4] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 5] = BLOB_UID_SIZE;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2 + 4, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "No data left for sigs" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2 + 4 + BLOB_UID_SIZE);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 2] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 3] = 1;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 4] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 5] = BLOB_UID_SIZE;
    assert_rnp_success(rnp_input_from_memory(
      &mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2 + 4 + BLOB_UID_SIZE, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too many sigs in the PGP blob" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2 + 4 + 4);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 2] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 3] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 4] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 5] = BLOB_UID_SIZE;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 6] = 0x80;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 7] = 1;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2 + 4 + 4, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too small SIGN structure" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2 + 4 + 4);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 2] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 3] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 4] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 5] = BLOB_UID_SIZE;
    /* [NSIGS] Number of signatures MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 6] = 0;
    /* [NSIGS] Number of signatures LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 7] = 1;
    /* Size of signature information MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 8] = 0;
    /* Size of signature information LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 9] = BLOB_SIG_SIZE - 1;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2 + 4 + 4, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too few data for sig" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2 + 4 + 4);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    buf[BLOB_FIRST_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 7] = 0;
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    buf[BLOB_FIRST_SIZE + 16] = 0;
    buf[BLOB_FIRST_SIZE + 17] = 1;
    buf[BLOB_FIRST_SIZE + 18] = 0;
    buf[BLOB_FIRST_SIZE + 19] = 28;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 2] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 3] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 4] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 5] = BLOB_UID_SIZE;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 7] = 1;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 8] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 9] = BLOB_SIG_SIZE;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2 + 4 + 4, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));
    /* "Too few data for trust/validities" */
    make_valid_header_blob(buf);
    w32(buf + BLOB_FIRST_SIZE, 20 + BLOB_KEY_SIZE + 2 + 4 + 4);
    buf[BLOB_FIRST_SIZE + 4] = KBX_PGP_BLOB;
    buf[BLOB_FIRST_SIZE + 5] = 1;
    /* flags MSB */
    buf[BLOB_FIRST_SIZE + 6] = 0;
    /* flags LSB */
    buf[BLOB_FIRST_SIZE + 7] = 0;
    /* keyblock offset */
    w32(buf + BLOB_FIRST_SIZE + 8, 0);
    /* keyblock length */
    w32(buf + BLOB_FIRST_SIZE + 12, 0);
    /* NKEYS MSB */
    buf[BLOB_FIRST_SIZE + 16] = 0;
    /* NKEYS LSB */
    buf[BLOB_FIRST_SIZE + 17] = 1;
    /* Size of the key information structure MSB */
    buf[BLOB_FIRST_SIZE + 18] = 0;
    /* Size of the key information structure LSB */
    buf[BLOB_FIRST_SIZE + 19] = 28;
    /* Size of the serial number MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE] = 0;
    /* Size of the serial number LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 1] = 0;
    /* Number of user IDs MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 2] = 0;
    /* Number of user IDs LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 3] = 0;
    /* Size of user ID information structure MSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 4] = 0;
    /* Size of user ID information structure LSB */
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 5] = BLOB_UID_SIZE;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 6] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 7] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 8] = 0;
    buf[BLOB_FIRST_SIZE + 20 + BLOB_KEY_SIZE + 9] = BLOB_SIG_SIZE;
    assert_rnp_success(
      rnp_input_from_memory(&mem_input, buf, BLOB_FIRST_SIZE + 20 + 28 + 2 + 4 + 4, false));
    assert_rnp_failure(rnp_load_keys(
      ffi, "KBX", mem_input, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS));
    assert_rnp_success(rnp_input_destroy(mem_input));

    rnp_ffi_destroy(ffi);
}
