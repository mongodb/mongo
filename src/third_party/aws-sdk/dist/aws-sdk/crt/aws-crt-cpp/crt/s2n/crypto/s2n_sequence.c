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

#include "crypto/s2n_sequence.h"

#include "error/s2n_errno.h"
#include "tls/s2n_crypto.h"
#include "utils/s2n_blob.h"

#define SEQUENCE_NUMBER_POWER 8

int s2n_increment_sequence_number(struct s2n_blob *sequence_number)
{
    for (uint32_t j = sequence_number->size; j > 0; j--) {
        uint32_t i = j - 1;
        sequence_number->data[i] += 1;
        if (sequence_number->data[i]) {
            break;
        }

        /* If a sequence number would exceed the maximum value, then we need to start a new session.
         * This condition is very unlikely. It requires 2^64 - 1 records to be sent.
         */
        S2N_ERROR_IF(i == 0, S2N_ERR_RECORD_LIMIT);

        /* seq[i] wrapped, so let it carry */
    }

    return 0;
}

int s2n_sequence_number_to_uint64(struct s2n_blob *sequence_number, uint64_t *output)
{
    POSIX_ENSURE_REF(sequence_number);

    uint8_t shift = 0;
    *output = 0;

    for (uint32_t i = sequence_number->size; i > 0; i--) {
        *output += ((uint64_t) sequence_number->data[i - 1]) << shift;
        shift += SEQUENCE_NUMBER_POWER;
    }
    return S2N_SUCCESS;
}
