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
#include <librepgp/stream-key.h>

TEST_F(rnp_tests, test_large_mpi_rsa_pub)
{
    pgp_source_t       keysrc = {0};
    pgp_key_sequence_t keyseq;
    rnp_ffi_t          ffi = NULL;
    rnp_input_t        input = NULL;
    rnp_input_t        signature = NULL;
    rnp_op_verify_t    verify;

    /* Load RSA pubkey packet with 65535 bit modulus MPI. Must fail. */
    assert_rnp_success(init_file_src(&keysrc, "data/test_large_MPIs/rsa-pub-65535bits.pgp"));
    assert_rnp_failure(process_pgp_keys(keysrc, keyseq, false));
    assert_true(keyseq.keys.empty());
    keysrc.close();

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/rsa-pub-65535bits.pgp"));
    assert_rnp_failure(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    rnp_input_destroy(input);

    /* Load RSA pubkey of PGP_MPINT_BITS + 1 size (16385). Must fail. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/rsa-pub-16385bits.pgp"));
    assert_rnp_failure(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    rnp_input_destroy(input);

    /* Load RSA pubkey of PGP_MPINT_BITS size (16384). Must succeed. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/rsa-pub-16384bits.pgp"));
    assert_rnp_success(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_PUBLIC_KEYS, NULL));
    rnp_input_destroy(input);

    /* Load RSA signature. rsa-pub-65535bits.pgp file signed by previously loaded 16384 bit RSA
     * key. Must succeed. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/rsa-pub-65535bits.pgp"));
    assert_rnp_success(
      rnp_input_from_path(&signature, "data/test_large_MPIs/rsa-pub-65535bits.pgp.sig"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, input, signature));
    assert_rnp_success(rnp_op_verify_execute(verify));
    rnp_input_destroy(input);
    rnp_input_destroy(signature);
    assert_rnp_success(rnp_op_verify_destroy(verify));

    /* Load RSA signature with PGP_MPINT_BITS + 1 size (16385) MPI. Must fail. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/rsa-pub-65535bits.pgp"));
    assert_rnp_success(rnp_input_from_path(
      &signature, "data/test_large_MPIs/rsa-pub-65535bits.pgp.16385sig.sig"));
    assert_rnp_success(rnp_op_verify_detached_create(&verify, ffi, input, signature));
    assert_rnp_failure(rnp_op_verify_execute(verify));
    rnp_input_destroy(input);
    rnp_input_destroy(signature);
    assert_rnp_success(rnp_op_verify_destroy(verify));

    rnp_ffi_destroy(ffi);
}

TEST_F(rnp_tests, test_large_mpi_rsa_priv)
{
    rnp_ffi_t    ffi = NULL;
    rnp_input_t  input = NULL;
    rnp_output_t output = NULL;

    assert_rnp_success(rnp_ffi_create(&ffi, "GPG", "GPG"));

    /* Load RSA private key of PGP_MPINT_BITS + 1 size (16385). Must fail. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/rsa-priv-16385bits.pgp"));
    assert_rnp_failure(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_SECRET_KEYS, NULL));
    rnp_input_destroy(input);

    /* Load RSA private key of PGP_MPINT_BITS size (16384). Must succeed. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/rsa-priv-16384bits.pgp"));
    assert_rnp_success(rnp_import_keys(ffi, input, RNP_LOAD_SAVE_SECRET_KEYS, NULL));
    rnp_input_destroy(input);

    /* Load PKESK encrypted by PGP_MPINT_BITS sized key. Must succeed. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/message.enc.rsa16384.pgp"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_success(rnp_decrypt(ffi, input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    /* Load PKESK having 16385 bit MPI. Must fail. */
    assert_rnp_success(
      rnp_input_from_path(&input, "data/test_large_MPIs/message.enc.rsa16385.pgp"));
    assert_rnp_success(rnp_output_to_null(&output));
    assert_rnp_failure(rnp_decrypt(ffi, input, output));
    rnp_input_destroy(input);
    rnp_output_destroy(output);

    rnp_ffi_destroy(ffi);
}
