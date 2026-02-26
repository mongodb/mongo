/*
 * Copyright (c) 2019 [Ribose Inc](https://www.ribose.com).
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
#include "keygen.hpp"

TEST_F(rnp_tests, test_key_prefs)
{
    rnp::UserPrefs pref1;
    rnp::UserPrefs pref2;

    /* symm algs */
    pref1.add_symm_alg(PGP_SA_AES_256);
    pref1.add_symm_alg(PGP_SA_AES_256);
    pref1.add_symm_alg(PGP_SA_AES_192);
    pref1.add_symm_alg(PGP_SA_AES_192);
    pref1.add_symm_alg(PGP_SA_AES_128);
    pref1.add_symm_alg(PGP_SA_AES_128);
    assert_int_equal(pref1.symm_algs.size(), 3);
    assert_int_equal(pref1.symm_algs[0], PGP_SA_AES_256);
    assert_int_equal(pref1.symm_algs[1], PGP_SA_AES_192);
    assert_int_equal(pref1.symm_algs[2], PGP_SA_AES_128);
    pref2.add_symm_alg(PGP_SA_CAMELLIA_128);
    pref2.add_symm_alg(PGP_SA_CAMELLIA_192);
    pref2.add_symm_alg(PGP_SA_CAMELLIA_256);
    pref1.symm_algs = pref2.symm_algs;
    assert_int_equal(pref1.symm_algs.size(), 3);
    assert_int_equal(pref1.symm_algs[0], PGP_SA_CAMELLIA_128);
    assert_int_equal(pref1.symm_algs[1], PGP_SA_CAMELLIA_192);
    assert_int_equal(pref1.symm_algs[2], PGP_SA_CAMELLIA_256);
    /* hash algs */
    pref1.add_hash_alg(PGP_HASH_SHA512);
    pref1.add_hash_alg(PGP_HASH_SHA384);
    pref1.add_hash_alg(PGP_HASH_SHA512);
    pref1.add_hash_alg(PGP_HASH_SHA384);
    pref1.add_hash_alg(PGP_HASH_SHA256);
    pref1.add_hash_alg(PGP_HASH_SHA256);
    assert_int_equal(pref1.hash_algs.size(), 3);
    assert_int_equal(pref1.hash_algs[0], PGP_HASH_SHA512);
    assert_int_equal(pref1.hash_algs[1], PGP_HASH_SHA384);
    assert_int_equal(pref1.hash_algs[2], PGP_HASH_SHA256);
    pref2.add_hash_alg(PGP_HASH_SHA3_512);
    pref2.add_hash_alg(PGP_HASH_SHA3_256);
    pref2.add_hash_alg(PGP_HASH_SHA1);
    pref1.hash_algs = pref2.hash_algs;
    assert_int_equal(pref1.hash_algs.size(), 3);
    assert_int_equal(pref1.hash_algs[0], PGP_HASH_SHA3_512);
    assert_int_equal(pref1.hash_algs[1], PGP_HASH_SHA3_256);
    assert_int_equal(pref1.hash_algs[2], PGP_HASH_SHA1);
    /* z algs */
    pref1.add_z_alg(PGP_C_ZIP);
    pref1.add_z_alg(PGP_C_ZLIB);
    pref1.add_z_alg(PGP_C_BZIP2);
    pref1.add_z_alg(PGP_C_ZIP);
    pref1.add_z_alg(PGP_C_ZLIB);
    pref1.add_z_alg(PGP_C_BZIP2);
    assert_int_equal(pref1.z_algs.size(), 3);
    assert_int_equal(pref1.z_algs[0], PGP_C_ZIP);
    assert_int_equal(pref1.z_algs[1], PGP_C_ZLIB);
    assert_int_equal(pref1.z_algs[2], PGP_C_BZIP2);
    pref2.add_z_alg(PGP_C_BZIP2);
    pref1.z_algs = pref2.z_algs;
    assert_int_equal(pref1.z_algs.size(), 1);
    assert_int_equal(pref1.z_algs[0], PGP_C_BZIP2);
    /* ks prefs */
    pref1.add_ks_pref(PGP_KEY_SERVER_NO_MODIFY);
    assert_int_equal(pref1.ks_prefs.size(), 1);
    assert_int_equal(pref1.ks_prefs[0], PGP_KEY_SERVER_NO_MODIFY);
    pref2.add_ks_pref((pgp_key_server_prefs_t) 0x20);
    pref2.add_ks_pref((pgp_key_server_prefs_t) 0x40);
    pref1.ks_prefs = pref2.ks_prefs;
    assert_int_equal(pref1.ks_prefs.size(), 2);
    assert_int_equal(pref1.ks_prefs[0], 0x20);
    assert_int_equal(pref1.ks_prefs[1], 0x40);
    /* ks url */
    pref1.key_server = "hkp://something/";
}
