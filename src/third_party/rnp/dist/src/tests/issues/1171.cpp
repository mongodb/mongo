/*
 * Copyright (c) 2020 [Ribose Inc](https://www.ribose.com).
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

#include "../rnp_tests.h"
#include "../support.h"
#include <librepgp/stream-ctx.h>
#include "key.hpp"
#include "ffi-priv-types.h"

TEST_F(rnp_tests, test_issue_1171_key_import_and_remove)
{
    rnp_ffi_t ffi = NULL;
    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sub-pub.pgp"));

    rnp_key_handle_t key = NULL;
    assert_rnp_success(
      rnp_locate_key(ffi, "grip", "3E3D52A346F0AD47754611B078117C240ED237E9", &key));
    assert_non_null(key);
    assert_rnp_success(rnp_key_remove(key, RNP_KEY_REMOVE_PUBLIC));
    assert_rnp_success(rnp_key_handle_destroy(key));

    assert_rnp_success(
      rnp_locate_key(ffi, "grip", "3E3D52A346F0AD47754611B078117C240ED237E9", &key));
    assert_null(key);

    assert_rnp_success(
      rnp_locate_key(ffi, "grip", "128E494F41F107E119AA1EEF7850C375A804341C", &key));
    assert_non_null(key);
    uint32_t bits = 0;
    assert_rnp_success(rnp_key_get_bits(key, &bits));
    assert_int_equal(bits, 256);

    /* directly use rnp_tests_get_key_by_grip() which caused crash */
    rnp::Key *subkey = rnp_tests_get_key_by_grip(ffi->pubring, key->pub->grip());
    assert_int_equal(subkey->material()->bits(), 256);
    assert_rnp_success(rnp_key_handle_destroy(key));

    assert_true(import_pub_keys(ffi, "data/test_key_validity/alice-sub-pub.pgp"));

    rnp_ffi_destroy(ffi);
}
