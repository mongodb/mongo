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

#pragma once

#include <stdint.h>

#include "crypto/s2n_hmac.h"
#include "stuffer/s2n_stuffer.h"

#define S2N_TLS_CONTENT_TYPE_LENGTH 1

#define S2N_TLS_SSLV2_HEADER_FLAG        (0x80)
#define S2N_TLS_SSLV2_HEADER_FLAG_UINT16 (S2N_TLS_SSLV2_HEADER_FLAG << 8)

/* All versions of TLS define the record header the same:
 * ContentType + ProtocolVersion + length
 */
#define S2N_TLS_RECORD_HEADER_LENGTH (S2N_TLS_CONTENT_TYPE_LENGTH + S2N_TLS_PROTOCOL_VERSION_LEN + 2)

/*
 * All versions of TLS limit the data fragment to 2^14 bytes.
 *
 *= https://www.rfc-editor.org/rfc/rfc5246#section-6.2.1
 *# The record layer fragments information blocks into TLSPlaintext
 *# records carrying data in chunks of 2^14 bytes or less.
 *
 *= https://www.rfc-editor.org/rfc/rfc8446#section-5.1
 *# The record layer fragments information blocks into TLSPlaintext
 *# records carrying data in chunks of 2^14 bytes or less.
 */
#define S2N_TLS_MAXIMUM_FRAGMENT_LENGTH (1 << 14)

/* The TLS1.2 record length allows for 1024 bytes of compression expansion and
 * 1024 bytes of encryption expansion and padding.
 * Since S2N does not support compression, we can ignore the compression overhead.
 */
#define S2N_TLS12_ENCRYPTION_OVERHEAD_SIZE 1024
#define S2N_TLS12_MAX_RECORD_LEN_FOR(frag) \
    ((frag) + S2N_TLS12_ENCRYPTION_OVERHEAD_SIZE + S2N_TLS_RECORD_HEADER_LENGTH)
#define S2N_TLS12_MAXIMUM_RECORD_LENGTH S2N_TLS12_MAX_RECORD_LEN_FOR(S2N_TLS_MAXIMUM_FRAGMENT_LENGTH)

/*
 *= https://www.rfc-editor.org/rfc/rfc8446#section-5.2
 *# An AEAD algorithm used in TLS 1.3 MUST NOT produce an expansion
 *# greater than 255 octets.
 */
#define S2N_TLS13_ENCRYPTION_OVERHEAD_SIZE 255
#define S2N_TLS13_MAX_RECORD_LEN_FOR(frag) ((frag) + S2N_TLS_CONTENT_TYPE_LENGTH \
        + S2N_TLS13_ENCRYPTION_OVERHEAD_SIZE                                     \
        + S2N_TLS_RECORD_HEADER_LENGTH)
#define S2N_TLS13_MAXIMUM_RECORD_LENGTH S2N_TLS13_MAX_RECORD_LEN_FOR(S2N_TLS_MAXIMUM_FRAGMENT_LENGTH)

/* Currently, TLS1.2 records may be larger than TLS1.3 records.
 * If the protocol is unknown, assume TLS1.2.
 */
#define S2N_TLS_MAX_RECORD_LEN_FOR(frag) S2N_TLS12_MAX_RECORD_LEN_FOR(frag)
#define S2N_TLS_MAXIMUM_RECORD_LENGTH    S2N_TLS_MAX_RECORD_LEN_FOR(S2N_TLS_MAXIMUM_FRAGMENT_LENGTH)

S2N_RESULT s2n_record_max_write_size(struct s2n_connection *conn, uint16_t max_fragment_size, uint16_t *max_record_size);
S2N_RESULT s2n_record_max_write_payload_size(struct s2n_connection *conn, uint16_t *max_fragment_size);
S2N_RESULT s2n_record_min_write_payload_size(struct s2n_connection *conn, uint16_t *payload_size);
S2N_RESULT s2n_record_write(struct s2n_connection *conn, uint8_t content_type, struct s2n_blob *in);
int s2n_record_writev(struct s2n_connection *conn, uint8_t content_type, const struct iovec *in, int in_count, size_t offs, size_t to_write);
int s2n_record_parse(struct s2n_connection *conn);
int s2n_record_header_parse(struct s2n_connection *conn, uint8_t *content_type, uint16_t *fragment_length);
int s2n_tls13_parse_record_type(struct s2n_stuffer *stuffer, uint8_t *record_type);
int s2n_sslv2_record_header_parse(struct s2n_connection *conn, uint8_t *record_type, uint8_t *client_protocol_version, uint16_t *fragment_length);
int s2n_verify_cbc(struct s2n_connection *conn, struct s2n_hmac_state *hmac, struct s2n_blob *decrypted);
S2N_RESULT s2n_aead_aad_init(const struct s2n_connection *conn, uint8_t *sequence_number, uint8_t content_type, uint16_t record_length, struct s2n_blob *ad);
S2N_RESULT s2n_tls13_aead_aad_init(uint16_t record_length, uint8_t tag_length, struct s2n_blob *ad);
S2N_RESULT s2n_record_wipe(struct s2n_connection *conn);
