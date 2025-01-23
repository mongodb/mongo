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

#include "crypto/s2n_evp.h"

#include "crypto/s2n_fips.h"
#include "error/s2n_errno.h"
#include "utils/s2n_safety.h"

int s2n_digest_allow_md5_for_fips(struct s2n_evp_digest *evp_digest)
{
    POSIX_ENSURE_REF(evp_digest);
    /* This is only to be used for EVP digests that will require MD5 to be used
     * to comply with the TLS 1.0 and 1.1 RFC's for the PRF. MD5 cannot be used
     * outside of the TLS 1.0 and 1.1 PRF when in FIPS mode.
     */
    S2N_ERROR_IF(!s2n_is_in_fips_mode() || (evp_digest->ctx == NULL), S2N_ERR_ALLOW_MD5_FOR_FIPS_FAILED);

#if !defined(OPENSSL_IS_BORINGSSL) && !defined(OPENSSL_IS_AWSLC)
    EVP_MD_CTX_set_flags(evp_digest->ctx, EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
#endif
    return S2N_SUCCESS;
}

S2N_RESULT s2n_digest_is_md5_allowed_for_fips(struct s2n_evp_digest *evp_digest, bool *out)
{
    RESULT_ENSURE_REF(out);
    *out = false;
#if !defined(OPENSSL_IS_BORINGSSL) && !defined(OPENSSL_IS_AWSLC)
    if (s2n_is_in_fips_mode() && evp_digest && evp_digest->ctx && EVP_MD_CTX_test_flags(evp_digest->ctx, EVP_MD_CTX_FLAG_NON_FIPS_ALLOW)) {
        /* s2n is in FIPS mode and the EVP digest allows MD5. */
        *out = true;
    }
#else
    if (s2n_is_in_fips_mode()) {
        /* If s2n is in FIPS mode and built with AWS-LC or BoringSSL, there are no flags to check in the EVP digest to allow MD5. */
        *out = true;
    }
#endif
    return S2N_RESULT_OK;
}
