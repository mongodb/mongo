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

#include "utils/s2n_init.h"
#include "utils/s2n_safety.h"

#if defined(S2N_INTERN_LIBCRYPTO) && defined(OPENSSL_FIPS)
    #error "Interning with OpenSSL fips-validated libcrypto is not currently supported. See https://github.com/aws/s2n-tls/issues/2741"
#endif

static bool s2n_fips_mode_enabled = false;

/* FIPS mode can be checked if OpenSSL was configured and built for FIPS which then defines OPENSSL_FIPS.
 *
 * AWS-LC always defines FIPS_mode() that you can call and check what the library was built with. It does not define
 * a public OPENSSL_FIPS/AWSLC_FIPS macro that we can (or need to) check here
 *
 * Safeguard with macro's, for example because Libressl dosn't define
 * FIPS_mode() by default.
 *
 * Note: FIPS_mode() does not change the FIPS state of libcrypto. This only returns the current state. Applications
 * using s2n must call FIPS_mode_set(1) prior to s2n_init.
 * */
bool s2n_libcrypto_is_fips(void)
{
#if defined(OPENSSL_FIPS) || defined(OPENSSL_IS_AWSLC)
    if (FIPS_mode() == 1) {
        return true;
    }
#endif
    return false;
}

int s2n_fips_init(void)
{
    s2n_fips_mode_enabled = s2n_libcrypto_is_fips();
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
