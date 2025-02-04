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

#include "utils/s2n_rfc5952.h"

#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "error/s2n_errno.h"
#include "utils/s2n_safety.h"

static uint8_t dec[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9' };
static uint8_t hex[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

S2N_RESULT s2n_inet_ntop(int af, const void *addr, struct s2n_blob *dst)
{
    const uint8_t *bytes = addr;
    uint8_t *cursor = dst->data;

    if (af == AF_INET) {
        RESULT_ENSURE(dst->size >= sizeof("111.222.333.444"), S2N_ERR_SIZE_MISMATCH);

        for (int i = 0; i < 4; i++) {
            if (bytes[i] / 100) {
                *cursor++ = dec[bytes[i] / 100];
            }
            if (bytes[i] >= 10) {
                *cursor++ = dec[(bytes[i] % 100) / 10];
            }
            *cursor++ = dec[(bytes[i] % 10)];
            *cursor++ = '.';
        }

        *--cursor = '\0';

        return S2N_RESULT_OK;
    }

    if (af == AF_INET6) {
        RESULT_ENSURE(dst->size >= sizeof("1111:2222:3333:4444:5555:6666:7777:8888"), S2N_ERR_SIZE_MISMATCH);

        /* See Section 4 of RFC5952 for the rules we are going to follow here
         *
         * Here's the general algorithm:
         *
         *   1/ Treat the bytes as 8 16-bit fields
         *   2/ Find the longest run of 16-bit fields.
         *   3/ or if there are two or more equal length longest runs, go with the left-most run
         *   4/ Make that run ::
         *   5/ Print the remaining 16-bit fields in lowercase hex, no leading zeroes
         */

        uint16_t octets[8] = { 0 };

        int longest_run_start = 0;
        int longest_run_length = 0;
        int current_run_length = 0;

        /* 2001:db8::1:0:0:1 */

        /* Find the longest run of zeroes */
        for (int i = 0; i < 8; i++) {
            octets[i] = (bytes[i * 2] << 8) + bytes[(i * 2) + 1];

            if (octets[i]) {
                current_run_length = 0;
            } else {
                current_run_length++;
            }

            if (current_run_length > longest_run_length) {
                longest_run_length = current_run_length;
                longest_run_start = (i - current_run_length) + 1;
            }
        }

        for (int i = 0; i < 8; i++) {
            if (i == longest_run_start && longest_run_length > 1) {
                if (i == 0) {
                    *cursor++ = ':';
                }

                if (longest_run_length == 8) {
                    *cursor++ = ':';
                }

                i += longest_run_length - 1;

            } else {
                uint8_t nibbles[4] = { (octets[i] & 0xF000) >> 12,
                    (octets[i] & 0x0F00) >> 8,
                    (octets[i] & 0x00F0) >> 4,
                    (octets[i] & 0x000F) };

                /* Skip up to three leading zeroes */
                int j = 0;
                for (j = 0; j < 3; j++) {
                    if (nibbles[j]) {
                        break;
                    }
                }

                for (; j < 4; j++) {
                    *cursor++ = hex[nibbles[j]];
                }
            }

            *cursor++ = ':';
        }

        *--cursor = '\0';

        return S2N_RESULT_OK;
    }

    RESULT_BAIL(S2N_ERR_INVALID_ARGUMENT);
}
