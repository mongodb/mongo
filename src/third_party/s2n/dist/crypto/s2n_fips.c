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

#include "crypto/s2n_fips.h"

#include <openssl/crypto.h>

#include "crypto/s2n_libcrypto.h"
#include "crypto/s2n_openssl.h"
#include "utils/s2n_init.h"
#include "utils/s2n_safety.h"

#if defined(S2N_INTERN_LIBCRYPTO) && defined(OPENSSL_FIPS)
    #error "Interning with OpenSSL fips-validated libcrypto is not currently supported. See https://github.com/aws/s2n-tls/issues/2741"
#endif

static bool s2n_fips_mode_enabled = false;

/* Check if the linked libcrypto has FIPS mode enabled.
 *
 * This method indicates the state of the libcrypto, NOT the state
 * of s2n-tls and should ONLY be called during library initialization (i.e.
 * s2n_init()). This distinction is important because in the past,
 * if s2n-tls was using Openssl-1.0.2-fips and FIPS_mode_set(1)
 * was called after s2n_init() was called, then this method would return true
 * while s2n_is_in_fips_mode() would return false and s2n-tls would not operate
 * in FIPS mode.
 *
 * For AWS-LC, the FIPS_mode() method is always defined. If AWS-LC was built to
 * support FIPS, FIPS_mode() always returns 1.
 */
bool s2n_libcrypto_is_fips(void)
{
#if defined(OPENSSL_FIPS) || defined(OPENSSL_IS_AWSLC)
    if (FIPS_mode() == 1) {
        return true;
    }
#elif S2N_OPENSSL_VERSION_AT_LEAST(3, 0, 0)
    return EVP_default_properties_is_fips_enabled(NULL);
#endif
    return false;
}

int s2n_fips_init(void)
{
    s2n_fips_mode_enabled = s2n_libcrypto_is_fips();

    /* When using Openssl, ONLY 3.0+ currently supports FIPS.
     * openssl-1.0.2-fips is no longer supported.
     */
#if defined(OPENSSL_FIPS)
    POSIX_ENSURE(!s2n_fips_mode_enabled, S2N_ERR_FIPS_MODE_UNSUPPORTED);
#endif

    return S2N_SUCCESS;
}

/* Return 1 if FIPS mode is enabled, 0 otherwise. FIPS mode must be enabled prior to calling s2n_init(). */
bool s2n_is_in_fips_mode(void)
{
    return s2n_fips_mode_enabled;
}

int s2n_get_fips_mode(s2n_fips_mode *fips_mode)
{
    POSIX_ENSURE_REF(fips_mode);
    *fips_mode = S2N_FIPS_MODE_DISABLED;
    POSIX_ENSURE(s2n_is_initialized(), S2N_ERR_NOT_INITIALIZED);

    if (s2n_is_in_fips_mode()) {
        *fips_mode = S2N_FIPS_MODE_ENABLED;
    }

    return S2N_SUCCESS;
}
