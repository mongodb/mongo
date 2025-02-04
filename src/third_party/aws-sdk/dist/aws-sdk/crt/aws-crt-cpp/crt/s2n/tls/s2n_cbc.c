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

#include <stdint.h>
#include <sys/param.h>

#include "crypto/s2n_hmac.h"
#include "error/s2n_errno.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_record.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

/* A TLS CBC record looks like ..
 *
 * [ Payload data ] [ HMAC ] [ Padding ] [ Padding length byte ]
 *
 * Each byte in the padding is expected to be set to the same value
 * as the padding length byte. So if the padding length byte is '2'
 * then the padding will be [ '2', '2' ] (there'll be three bytes
 * set to that value if you include the padding length byte).
 *
 * The goal of s2n_verify_cbc() is to verify that the padding and hmac
 * are correct, without leaking (via timing) how much padding there
 * actually is: as this is considered secret. 
 *
 * In addition to our efforts here though, s2n also wraps any CBC
 * verification error (or record parsing error in general) with
 * a randomized delay of between 1ms and 10 seconds. See s2n_connection.c.
 * This amount of delay randomization is sufficient to increase the
 * complexity of attack for even a 1 microsecond timing leak (which
 * is quite large) by a factor of around 83 trillion.
 */
int s2n_verify_cbc(struct s2n_connection *conn, struct s2n_hmac_state *hmac, struct s2n_blob *decrypted)
{
    uint8_t mac_digest_size = 0;
    POSIX_GUARD(s2n_hmac_digest_size(hmac->alg, &mac_digest_size));

    /* The record has to be at least big enough to contain the MAC,
     * plus the padding length byte */
    POSIX_ENSURE_GT(decrypted->size, mac_digest_size);

    int payload_and_padding_size = decrypted->size - mac_digest_size;

    /* Determine what the padding length is */
    uint8_t padding_length = decrypted->data[decrypted->size - 1];

    int payload_length = MAX(payload_and_padding_size - padding_length - 1, 0);

    /* Update the MAC */
    POSIX_GUARD(s2n_hmac_update(hmac, decrypted->data, payload_length));
    int currently_in_hash_block = hmac->currently_in_hash_block;

    /* Check the MAC */
    uint8_t check_digest[S2N_MAX_DIGEST_LEN];
    POSIX_ENSURE_LTE(mac_digest_size, sizeof(check_digest));
    POSIX_GUARD(s2n_hmac_digest_two_compression_rounds(hmac, check_digest, mac_digest_size));

    int mismatches = s2n_constant_time_equals(decrypted->data + payload_length, check_digest, mac_digest_size) ^ 1;

    /* Compute a MAC on the rest of the data so that we perform the same number of hash operations.
     * Include the partial hash block from the first MAC to ensure we use the same number of blocks.
     */
    POSIX_GUARD(s2n_hmac_reset(hmac));
    POSIX_GUARD(s2n_hmac_update(hmac, decrypted->data, currently_in_hash_block));
    POSIX_GUARD(s2n_hmac_update(hmac, decrypted->data + payload_length + mac_digest_size, decrypted->size - payload_length - mac_digest_size - 1));

    /* SSLv3 doesn't specify what the padding should actually be */
    if (conn->actual_protocol_version == S2N_SSLv3) {
        return 0 - mismatches;
    }

    /* Check the maximum amount that could theoretically be padding */
    uint32_t check = MIN(255, (payload_and_padding_size - 1));

    POSIX_ENSURE_GTE(check, padding_length);

    uint32_t cutoff = check - padding_length;
    for (size_t i = 0, j = decrypted->size - 1 - check; i < check && j < decrypted->size; i++, j++) {
        uint8_t mask = ~(0xff << ((i >= cutoff) * 8));
        mismatches |= (decrypted->data[j] ^ padding_length) & mask;
    }

    S2N_ERROR_IF(mismatches, S2N_ERR_CBC_VERIFY);

    return 0;
}
