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

/**
 * openssl with OPENSSL_VERSION_NUMBER < 0x10100003L made data type details unavailable
 * libressl use openssl with data type details available, but mandatorily set
 * OPENSSL_VERSION_NUMBER = 0x20000000L, insane!
 * https://github.com/aws/aws-sdk-cpp/pull/507/commits/2c99f1fe0c4b4683280caeb161538d4724d6a179
 */
#if defined(LIBRESSL_VERSION_NUMBER) && (OPENSSL_VERSION_NUMBER == 0x20000000L)
    #undef OPENSSL_VERSION_NUMBER
    #if LIBRESSL_VERSION_NUMBER < 0x3050000fL
        #define OPENSSL_VERSION_NUMBER 0x1000107fL
    #else
        #define OPENSSL_VERSION_NUMBER 0x1010000fL
    #endif
#endif

/* Per https://wiki.openssl.org/index.php/Manual:OPENSSL_VERSION_NUMBER(3)
 * OPENSSL_VERSION_NUMBER in hex is: MNNFFRBB major minor fix final beta/patch.
 * bitwise: MMMMNNNNNNNNFFFFFFFFRRRRBBBBBBBB
 * For our purposes we're only concerned about major/minor/fix. Patch versions don't usually introduce
 * features.
 */

#define S2N_OPENSSL_VERSION_AT_LEAST(major, minor, fix) \
    (OPENSSL_VERSION_NUMBER >= ((major << 28) + (minor << 20) + (fix << 12)))

#if (S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0)) && (!defined(OPENSSL_IS_BORINGSSL)) && (!defined(OPENSSL_IS_AWSLC)) && (!defined(LIBRESSL_VERSION_NUMBER))
    #define s2n_evp_ctx_init(ctx)    POSIX_GUARD_OSSL(EVP_CIPHER_CTX_init(ctx), S2N_ERR_RANDOM)
    #define RESULT_EVP_CTX_INIT(ctx) RESULT_GUARD_OSSL(EVP_CIPHER_CTX_init(ctx), S2N_ERR_RANDOM)
#else
    #define s2n_evp_ctx_init(ctx)    EVP_CIPHER_CTX_init(ctx)
    #define RESULT_EVP_CTX_INIT(ctx) EVP_CIPHER_CTX_init(ctx)
#endif
