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

#include "s2n_pq.h"

#include "crypto/s2n_openssl.h"

bool s2n_libcrypto_supports_evp_kem()
{
    /* S2N_LIBCRYPTO_SUPPORTS_EVP_KEM will be auto-detected and #defined if
     * ./tests/features/S2N_LIBCRYPTO_SUPPORTS_EVP_KEM.c successfully compiles
     */
#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_KEM)
    return true;
#else
    return false;
#endif
}

bool s2n_pq_is_enabled()
{
    return s2n_libcrypto_supports_evp_kem();
}

bool s2n_libcrypto_supports_mlkem()
{
    /* S2N_LIBCRYPTO_SUPPORTS_MLKEM will be auto-detected and #defined if
     * ./tests/features/S2N_LIBCRYPTO_SUPPORTS_MLKEM.c successfully compiles
     */
#if defined(S2N_LIBCRYPTO_SUPPORTS_MLKEM)
    return true;
#else
    return false;
#endif
}
