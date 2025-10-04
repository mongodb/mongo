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

#include "crypto/s2n_libcrypto.h"

#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <string.h>

#include "crypto/s2n_crypto.h"
#include "crypto/s2n_fips.h"
#include "crypto/s2n_openssl.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_safety_macros.h"

/* Note: OpenSSL 1.0.2 -> 1.1.0 implemented a new API to get the version number
 * and version name. We have to handle that by using old functions
 * (named "SSLea*"). Newer version of OpenSSL luckily define these symbols to
 * the new API. When dropping OpenSSL 1.0.2 support, we can move to the new API.
 */

/* The result of SSLeay_version(SSLEAY_VERSION) for OpenSSL and AWS-LC depends on the
 * version. AWS-LC and BoringSSL have consistent prefixes that can be statically asserted.
 *
 * https://github.com/awslabs/aws-lc/commit/8f184f5d69604cc4645bafec47c2d6d9929cb50f
 * has not been pushed to the fips branch of AWS-LC. In addition, we can't
 * distinguish AWS-LC fips and non-fips at pre-processing time since AWS-LC
 * doesn't distribute fips-specific header files.
 */
#define EXPECTED_AWSLC_VERSION_PREFIX_OLD "BoringSSL"
#define EXPECTED_AWSLC_VERSION_PREFIX_NEW "AWS-LC"
#define EXPECTED_BORINGSSL_VERSION_PREFIX "BoringSSL"

/* https://www.openssl.org/docs/man{1.0.2, 1.1.1, 3.0}/man3/OPENSSL_VERSION_NUMBER.html
 * OPENSSL_VERSION_NUMBER in hex is: MNNFFPPS major minor fix patch status.
 * Bitwise: MMMMNNNNNNNNFFFFFFFFPPPPPPPPSSSS
 * To not be overly restrictive, we only care about the major version.
 * From OpenSSL 3.0 the "fix" part is also deprecated and is always a flat 0x00.
 */
#define VERSION_NUMBER_MASK 0xF0000000L

/* Returns the version name of the libcrypto containing the definition that the
 * symbol OpenSSL_version binded to at link-time. This can be used as
 * verification at run-time that s2n linked against the expected libcrypto.
 */
const char *s2n_libcrypto_get_version_name(void)
{
    return SSLeay_version(SSLEAY_VERSION);
}

static S2N_RESULT s2n_libcrypto_validate_expected_version_prefix(const char *expected_name_prefix)
{
    RESULT_ENSURE_REF(expected_name_prefix);
    RESULT_ENSURE_REF(s2n_libcrypto_get_version_name());
    RESULT_ENSURE_LTE(strlen(expected_name_prefix), strlen(s2n_libcrypto_get_version_name()));
    RESULT_ENSURE(s2n_constant_time_equals((const uint8_t *) expected_name_prefix, (const uint8_t *) s2n_libcrypto_get_version_name(), (const uint32_t) strlen(expected_name_prefix)), S2N_ERR_LIBCRYPTO_VERSION_NAME_MISMATCH);

    return S2N_RESULT_OK;
}

/* Compare compile-time version number with the version number of the libcrypto
 * containing the definition that the symbol OpenSSL_version_num binded to at
 * link-time.
 *
 * This is an imperfect check for AWS-LC and BoringSSL, since their version
 * number is basically never incremented. However, for these we have a strong
 * check through s2n_libcrypto_validate_expected_version_name(), so it is not
 * of great importance.
 */
static S2N_RESULT s2n_libcrypto_validate_expected_version_number(void)
{
/* We mutate the version number in s2n_openssl.h when detecting Libressl. This
 * value is cached by s2n_get_openssl_version(). Hence, for libressl, the
 * run-time version number will always be different from what
 * s2n_get_openssl_version() returns. We cater for this here by just getting
 * what ever we cached instead of asking Libressl libcrypto.
 */
#if defined(LIBRESSL_VERSION_NUMBER)
    unsigned long run_time_version_number = s2n_get_openssl_version() & VERSION_NUMBER_MASK;
#else
    unsigned long run_time_version_number = SSLeay() & VERSION_NUMBER_MASK;
#endif
    unsigned long compile_time_version_number = s2n_get_openssl_version() & VERSION_NUMBER_MASK;
    RESULT_ENSURE(compile_time_version_number == run_time_version_number, S2N_ERR_LIBCRYPTO_VERSION_NUMBER_MISMATCH);

    return S2N_RESULT_OK;
}

/* s2n_libcrypto_is_*() encodes the libcrypto version used at build-time.
 * Currently only captures AWS-LC and BoringSSL. When a libcrypto-dependent
 * branch is required, we prefer these functions where possible to reduce
 # #ifs and avoid potential bugs where the header containing the #define is not
 * included.
 */

#if defined(OPENSSL_IS_AWSLC) && defined(OPENSSL_IS_BORINGSSL)
    #error "Both OPENSSL_IS_AWSLC and OPENSSL_IS_BORINGSSL are defined at the same time!"
#endif

bool s2n_libcrypto_is_awslc()
{
#if defined(OPENSSL_IS_AWSLC)
    return true;
#else
    return false;
#endif
}

uint64_t s2n_libcrypto_awslc_api_version(void)
{
#if defined(OPENSSL_IS_AWSLC)
    return AWSLC_API_VERSION;
#else
    return 0;
#endif
}

bool s2n_libcrypto_is_boringssl()
{
#if defined(OPENSSL_IS_BORINGSSL)
    return true;
#else
    return false;
#endif
}

bool s2n_libcrypto_is_libressl()
{
#if defined(LIBRESSL_VERSION_NUMBER)
    return true;
#else
    return false;
#endif
}

/* Performs various checks to validate that the libcrypto used at compile-time
 * is the same libcrypto being used at run-time.
 */
S2N_RESULT s2n_libcrypto_validate_runtime(void)
{
    /* Sanity check that we don't think we built against AWS-LC and BoringSSL at
     * the same time.
     */
    RESULT_ENSURE_EQ(s2n_libcrypto_is_boringssl() && s2n_libcrypto_is_awslc(), false);

    /* If we know the expected version name, we can validate it. */
    if (s2n_libcrypto_is_awslc()) {
        const char *expected_awslc_name_prefix = NULL;
        /* For backwards compatability, also check the AWS-LC API version see
         * https://github.com/awslabs/aws-lc/pull/467. When we are confident we
         * don't meet anymore "old" AWS-LC libcrypto's, this API version check
         * can be removed.
         */
        if (s2n_libcrypto_awslc_api_version() < 17) {
            expected_awslc_name_prefix = EXPECTED_AWSLC_VERSION_PREFIX_OLD;
        } else {
            expected_awslc_name_prefix = EXPECTED_AWSLC_VERSION_PREFIX_NEW;
        }
        RESULT_GUARD(s2n_libcrypto_validate_expected_version_prefix(expected_awslc_name_prefix));
    } else if (s2n_libcrypto_is_boringssl()) {
        RESULT_GUARD(s2n_libcrypto_validate_expected_version_prefix(EXPECTED_BORINGSSL_VERSION_PREFIX));
    }

    RESULT_GUARD(s2n_libcrypto_validate_expected_version_number());

    return S2N_RESULT_OK;
}

bool s2n_libcrypto_is_interned(void)
{
#if defined(S2N_INTERN_LIBCRYPTO)
    return true;
#else
    return false;
#endif
}

unsigned long s2n_get_openssl_version(void)
{
    return OPENSSL_VERSION_NUMBER;
}

bool s2n_libcrypto_supports_flag_no_check_time()
{
#ifdef S2N_LIBCRYPTO_SUPPORTS_FLAG_NO_CHECK_TIME
    return true;
#else
    return false;
#endif
}
