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

#include "error/s2n_errno.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_record.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

/* Derive the AAD for an AEAD mode cipher suite from the connection state, per
 * RFC 5246 section 6.2.3.3 */
S2N_RESULT s2n_aead_aad_init(const struct s2n_connection *conn, uint8_t *sequence_number, uint8_t content_type, uint16_t record_length, struct s2n_blob *ad)
{
    RESULT_ENSURE_REF(ad);
    RESULT_ENSURE_GTE(ad->size, S2N_TLS_MAX_AAD_LEN);

    uint8_t *data = ad->data;
    RESULT_GUARD_PTR(data);

    /* ad = seq_num || record_type || version || length */

    size_t idx = 0;
    for (; idx < S2N_TLS_SEQUENCE_NUM_LEN; idx++) {
        data[idx] = sequence_number[idx];
    }

    data[idx++] = content_type;
    data[idx++] = conn->actual_protocol_version / 10;
    data[idx++] = conn->actual_protocol_version % 10;
    data[idx++] = record_length >> 8;
    data[idx++] = record_length & UINT8_MAX;

    /* Double check no overflow */
    RESULT_ENSURE_LTE(idx, ad->size);
    return S2N_RESULT_OK;
}

/* Prepares an AAD (additional authentication data) for a TLS 1.3 AEAD record */
S2N_RESULT s2n_tls13_aead_aad_init(uint16_t record_length, uint8_t tag_length, struct s2n_blob *additional_data)
{
    RESULT_ENSURE_GT(tag_length, 0);
    RESULT_ENSURE_REF(additional_data);
    RESULT_ENSURE_GTE(additional_data->size, S2N_TLS13_AAD_LEN);

    uint8_t *data = additional_data->data;
    RESULT_GUARD_PTR(data);

    size_t idx = 0;

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-5.2
     *# opaque_type:  The outer opaque_type field of a TLSCiphertext record
     *#    is always set to the value 23 (application_data) for outward
     *#    compatibility with middleboxes accustomed to parsing previous
     *#    versions of TLS.  The actual content type of the record is found
     *#    in TLSInnerPlaintext.type after decryption.
     **/
    data[idx++] = TLS_APPLICATION_DATA;

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-5.2
     *# legacy_record_version:  The legacy_record_version field is always
     *#    0x0303.  TLS 1.3 TLSCiphertexts are not generated until after
     *#    TLS 1.3 has been negotiated, so there are no historical
     *#    compatibility concerns where other values might be received.  Note
     *#    that the handshake protocol, including the ClientHello and
     *#    ServerHello messages, authenticates the protocol version, so this
     *#    value is redundant.
     */
    data[idx++] = 0x03;
    data[idx++] = 0x03;

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-5.2
     *# length:  The length (in bytes) of the following
     *#    TLSCiphertext.encrypted_record, which is the sum of the lengths of
     *#    the content and the padding, plus one for the inner content type,
     *#    plus any expansion added by the AEAD algorithm.  The length
     *#    MUST NOT exceed 2^14 + 256 bytes.  An endpoint that receives a
     *#    record that exceeds this length MUST terminate the connection with
     *#    a "record_overflow" alert.
     */
    uint16_t length = record_length + tag_length;
    RESULT_ENSURE(length <= (1 << 14) + 256, S2N_ERR_RECORD_LIMIT);
    data[idx++] = length >> 8;
    data[idx++] = length & UINT8_MAX;

    /* Double check no overflow */
    RESULT_ENSURE_LTE(idx, additional_data->size);
    return S2N_RESULT_OK;
}
