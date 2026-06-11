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

#include "crypto/s2n_rsa_pss.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <stdint.h>

#include "crypto/s2n_hash.h"
#include "crypto/s2n_pkey.h"
#include "crypto/s2n_pkey_evp.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"

/* Checks whether PSS Certs is supported */
bool s2n_is_rsa_pss_certs_supported()
{
    return RSA_PSS_CERTS_SUPPORTED;
}

bool s2n_is_rsa_pss_signing_supported()
{
#if defined(S2N_LIBCRYPTO_SUPPORTS_RSA_PSS_SIGNING)
    return true;
#else
    return false;
#endif
}
