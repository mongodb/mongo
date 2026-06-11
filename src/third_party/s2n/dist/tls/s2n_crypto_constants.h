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

#define S2N_TLS_SECRET_LEN         48
#define S2N_TLS_RANDOM_DATA_LEN    32
#define S2N_TLS_SEQUENCE_NUM_LEN   8
#define S2N_TLS_CIPHER_SUITE_LEN   2
#define S2N_SSLv2_CIPHER_SUITE_LEN 3
#define S2N_TLS_FINISHED_LEN       12
#define S2N_SSL_FINISHED_LEN       36
#define S2N_TLS_MAX_IV_LEN         16

/* From RFC 5246 6.2.3.3 */
#define S2N_TLS12_AAD_LEN           13
#define S2N_TLS_MAX_AAD_LEN         S2N_TLS12_AAD_LEN
#define S2N_TLS_GCM_FIXED_IV_LEN    4
#define S2N_TLS_GCM_EXPLICIT_IV_LEN 8
#define S2N_TLS_GCM_IV_LEN          (S2N_TLS_GCM_FIXED_IV_LEN + S2N_TLS_GCM_EXPLICIT_IV_LEN)
#define S2N_TLS_GCM_TAG_LEN         16
#define S2N_TLS_AES_128_GCM_KEY_LEN 16
#define S2N_TLS_AES_256_GCM_KEY_LEN 32

/* TLS 1.3 uses only implicit IVs - RFC 8446 5.3 */
#define S2N_TLS13_AAD_LEN       5
#define S2N_TLS13_RECORD_IV_LEN 0
#define S2N_TLS13_FIXED_IV_LEN  12

/* From RFC 7905 */
#define S2N_TLS_CHACHA20_POLY1305_FIXED_IV_LEN    12
#define S2N_TLS_CHACHA20_POLY1305_EXPLICIT_IV_LEN 0
#define S2N_TLS_CHACHA20_POLY1305_IV_LEN          12
#define S2N_TLS_CHACHA20_POLY1305_KEY_LEN         32
#define S2N_TLS_CHACHA20_POLY1305_TAG_LEN         16

/* RFC 5246 7.4.1.2 */
#define S2N_TLS_SESSION_ID_MAX_LEN 32
