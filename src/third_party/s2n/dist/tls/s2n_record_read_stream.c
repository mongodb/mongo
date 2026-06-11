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
#include "tls/s2n_record_read.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

int s2n_record_parse_stream(
        const struct s2n_cipher_suite *cipher_suite,
        struct s2n_connection *conn,
        uint8_t content_type,
        uint16_t encrypted_length,
        uint8_t *implicit_iv,
        struct s2n_hmac_state *mac,
        uint8_t *sequence_number,
        struct s2n_session_key *session_key)
{
    /* Add the header to the HMAC */
    uint8_t *header = s2n_stuffer_raw_read(&conn->header_in, S2N_TLS_RECORD_HEADER_LENGTH);
    POSIX_ENSURE_REF(header);

    struct s2n_blob en = { .size = encrypted_length, .data = s2n_stuffer_raw_read(&conn->in, encrypted_length) };
    POSIX_ENSURE_REF(en.data);

    uint16_t payload_length = encrypted_length;
    uint8_t mac_digest_size = 0;
    POSIX_GUARD(s2n_hmac_digest_size(mac->alg, &mac_digest_size));

    POSIX_ENSURE_GTE(payload_length, mac_digest_size);
    payload_length -= mac_digest_size;

    /* Decrypt stuff! */
    POSIX_GUARD(cipher_suite->record_alg->cipher->io.stream.decrypt(session_key, &en, &en));

    /* Update the MAC */
    header[3] = (payload_length >> 8);
    header[4] = payload_length & 0xff;
    POSIX_GUARD(s2n_hmac_reset(mac));
    POSIX_GUARD(s2n_hmac_update(mac, sequence_number, S2N_TLS_SEQUENCE_NUM_LEN));

    if (conn->actual_protocol_version == S2N_SSLv3) {
        POSIX_GUARD(s2n_hmac_update(mac, header, 1));
        POSIX_GUARD(s2n_hmac_update(mac, header + 3, 2));
    } else {
        POSIX_GUARD(s2n_hmac_update(mac, header, S2N_TLS_RECORD_HEADER_LENGTH));
    }

    struct s2n_blob seq = { .data = sequence_number, .size = S2N_TLS_SEQUENCE_NUM_LEN };
    POSIX_GUARD(s2n_increment_sequence_number(&seq));

    /* MAC check for streaming ciphers - no padding */
    POSIX_GUARD(s2n_hmac_update(mac, en.data, payload_length));

    uint8_t check_digest[S2N_MAX_DIGEST_LEN];
    POSIX_ENSURE_LTE(mac_digest_size, sizeof(check_digest));
    POSIX_GUARD(s2n_hmac_digest(mac, check_digest, mac_digest_size));

    if (s2n_hmac_digest_verify(en.data + payload_length, check_digest, mac_digest_size) < 0) {
        POSIX_BAIL(S2N_ERR_BAD_MESSAGE);
    }

    /* O.k., we've successfully read and decrypted the record, now we need to align the stuffer
     * for reading the plaintext data.
     */
    POSIX_GUARD(s2n_stuffer_reread(&conn->in));
    POSIX_GUARD(s2n_stuffer_reread(&conn->header_in));

    /* Truncate and wipe the MAC and any padding */
    POSIX_GUARD(s2n_stuffer_wipe_n(&conn->in, s2n_stuffer_data_available(&conn->in) - payload_length));
    conn->in_status = PLAINTEXT;

    return 0;
}
