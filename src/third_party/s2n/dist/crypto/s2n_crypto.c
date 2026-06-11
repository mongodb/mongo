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

#include "crypto/s2n_crypto.h"

#include <stdint.h>

#include "api/s2n.h"

/* OPENSSL_free is defined within <openssl/crypto.h> for OpenSSL Libcrypto
 * and within <openssl/mem.h> for AWS_LC and BoringSSL */
#if defined(OPENSSL_IS_BORINGSSL) || defined(OPENSSL_IS_AWSLC)
    #include <openssl/mem.h>
#else
    #include <openssl/crypto.h>
#endif

int s2n_crypto_free(uint8_t** data)
{
    if (*data != NULL) {
        OPENSSL_free(*data);
    }
    return S2N_SUCCESS;
}
