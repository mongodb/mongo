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

#include <openssl/bn.h>
#include <stdbool.h>
#include <stdint.h>

#include "crypto/s2n_openssl.h"

#define RSA_PSS_SIGN_VERIFY_RANDOM_BLOB_SIZE 32
#define RSA_PSS_SIGN_VERIFY_SIGNATURE_SIZE   256

/* OpenSSL 1.1.1d  10 Sep 2019 is broken, so disable on that version. For further info see: crypto/evp/p_lib.c:469
 *
 * This feature requires this Openssl commit for Openssl 1.1.x versions: openssl/openssl@4088b92
 */
#if defined(S2N_LIBCRYPTO_SUPPORTS_RSA_PSS_SIGNING) && OPENSSL_VERSION_NUMBER > 0x1010104fL
    #define RSA_PSS_CERTS_SUPPORTED 1
#else
    #define RSA_PSS_CERTS_SUPPORTED 0
#endif

bool s2n_is_rsa_pss_certs_supported();
bool s2n_is_rsa_pss_signing_supported();
