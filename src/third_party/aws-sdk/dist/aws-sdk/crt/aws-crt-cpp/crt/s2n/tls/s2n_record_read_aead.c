/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "crypto/s2n_cipher.h"
#include "crypto/s2n_hmac.h"
#include "crypto/s2n_sequence.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_crypto.h"
#include "tls/s2n_record.h"
#include "tls/s2n_record_read.h"
#include "utils/s2n_annotations.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

int s2n_record_parse_aead(
        const struct s2n_cipher_suite *cipher_suite,
        struct s2n_connection *conn,
        uint8_t content_type,
        uint16_t encrypted_length,
        uint8_t *implicit_iv,
        struct s2n_hmac_state *mac,
        uint8_t *sequence_number,
        struct s2n_session_key *session_key)
{
    const int is_tls13_record = cipher_suite->record_alg->flags & S2N_TLS13_RECORD_AEAD_NONCE;
    /* TLS 1.3 record protection uses a different 5 byte associated data than TLS 1.2's */
    s2n_stack_blob(aad, is_tls13_record ? S2N_TLS13_AAD_LEN : S2N_TLS_MAX_AAD_LEN, S2N_TLS_MAX_AAD_LEN);

    struct s2n_blob en = { 0 };
    POSIX_GUARD(s2n_blob_init(&en, s2n_stuffer_raw_read(&conn->in, encrypted_length), encrypted_length));
    POSIX_ENSURE_REF(en.data);
    /* In AEAD mode, the explicit IV is in the record */
    POSIX_ENSURE_GTE(en.size, cipher_suite->record_alg->cipher->io.aead.record_iv_size);

    uint8_t aad_iv[S2N_TLS_MAX_IV_LEN] = { 0 };
    struct s2n_blob iv = { 0 };
    POSIX_GUARD(s2n_blob_init(&iv, aad_iv, sizeof(aad_iv)));
    struct s2n_stuffer iv_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&iv_stuffer, &iv));

    if (cipher_suite->record_alg->flags & S2N_TLS12_AES_GCM_AEAD_NONCE) {
        /* Partially explicit nonce. See RFC 5288 Section 3 */
        POSIX_GUARD(s2n_stuffer_write_bytes(&iv_stuffer, implicit_iv, cipher_suite->record_alg->cipher->io.aead.fixed_iv_size));
        POSIX_GUARD(s2n_stuffer_write_bytes(&iv_stuffer, en.data, cipher_suite->record_alg->cipher->io.aead.record_iv_size));
    } else if (cipher_suite->record_alg->flags & S2N_TLS12_CHACHA_POLY_AEAD_NONCE || is_tls13_record) {
        /* Fully implicit nonce.
         * This is introduced with ChaChaPoly with RFC 7905 Section 2
         * and also used for TLS 1.3 record protection (RFC 8446 Section 5.2).
         *
         * In these cipher modes, the sequence number (64 bits) is left padded by 4 bytes
         * to align and xor-ed with the 96-bit IV.
         **/
        uint8_t four_zeroes[4] = { 0 };
        POSIX_GUARD(s2n_stuffer_write_bytes(&iv_stuffer, four_zeroes, 4));
        POSIX_GUARD(s2n_stuffer_write_bytes(&iv_stuffer, sequence_number, S2N_TLS_SEQUENCE_NUM_LEN));
        for (int i = 0; i < cipher_suite->record_alg->cipher->io.aead.fixed_iv_size; i++) {
            S2N_INVARIANT(i <= cipher_suite->record_alg->cipher->io.aead.fixed_iv_size);
            aad_iv[i] = aad_iv[i] ^ implicit_iv[i];
        }
    } else {
        POSIX_BAIL(S2N_ERR_INVALID_NONCE_TYPE);
    }

    /* Set the IV size to the amount of data written */
    iv.size = s2n_stuffer_data_available(&iv_stuffer);

    uint16_t payload_length = encrypted_length;
    /* remove the AEAD overhead from the record size */
    POSIX_ENSURE_GTE(payload_length, cipher_suite->record_alg->cipher->io.aead.record_iv_size + cipher_suite->record_alg->cipher->io.aead.tag_size);
    payload_length -= cipher_suite->record_alg->cipher->io.aead.record_iv_size;
    payload_length -= cipher_suite->record_alg->cipher->io.aead.tag_size;

    if (is_tls13_record) {
        POSIX_GUARD_RESULT(s2n_tls13_aead_aad_init(payload_length, cipher_suite->record_alg->cipher->io.aead.tag_size, &aad));
    } else {
        POSIX_GUARD_RESULT(s2n_aead_aad_init(conn, sequence_number, content_type, payload_length, &aad));
    }

    /* Decrypt stuff! */
    /* Skip explicit IV for decryption */
    en.size -= cipher_suite->record_alg->cipher->io.aead.record_iv_size;
    en.data += cipher_suite->record_alg->cipher->io.aead.record_iv_size;

    /* Check that we have some data to decrypt */
    POSIX_ENSURE_NE(en.size, 0);

    POSIX_GUARD(cipher_suite->record_alg->cipher->io.aead.decrypt(session_key, &iv, &aad, &en, &en));
    struct s2n_blob seq = { 0 };
    POSIX_GUARD(s2n_blob_init(&seq, sequence_number, S2N_TLS_SEQUENCE_NUM_LEN));
    POSIX_GUARD(s2n_increment_sequence_number(&seq));

    /* O.k., we've successfully read and decrypted the record, now we need to align the stuffer
     * for reading the plaintext data.
     */
    POSIX_GUARD(s2n_stuffer_reread(&conn->in));
    POSIX_GUARD(s2n_stuffer_reread(&conn->header_in));

    /* Skip the IV, if any */
    if (conn->actual_protocol_version >= S2N_TLS12) {
        POSIX_GUARD(s2n_stuffer_skip_read(&conn->in, cipher_suite->record_alg->cipher->io.aead.record_iv_size));
    }

    /* Truncate and wipe the MAC and any padding */
    POSIX_GUARD(s2n_stuffer_wipe_n(&conn->in, s2n_stuffer_data_available(&conn->in) - payload_length));
    conn->in_status = PLAINTEXT;

    return 0;
}
