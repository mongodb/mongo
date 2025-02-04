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

#include "tls/s2n_ecc_preferences.h"

#include "api/s2n.h"
#include "crypto/s2n_ecc_evp.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_safety.h"

/* Chosen based on AWS server recommendations as of 05/24:
 * - All supported curves
 * - Prefer p256
 */
const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_20240501[] = {
    &s2n_ecc_curve_secp256r1,
#if EVP_APIS_SUPPORTED
    &s2n_ecc_curve_x25519,
#endif
    &s2n_ecc_curve_secp384r1,
    &s2n_ecc_curve_secp521r1,
};

const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_20140601[] = {
    &s2n_ecc_curve_secp256r1,
    &s2n_ecc_curve_secp384r1,
};

const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_20200310[] = {
#if EVP_APIS_SUPPORTED
    &s2n_ecc_curve_x25519,
#endif
    &s2n_ecc_curve_secp256r1,
    &s2n_ecc_curve_secp384r1,
};

/* Curve p256 is at the top of the list in order to minimize HRR */
const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_20230623[] = {
    &s2n_ecc_curve_secp256r1,
#if EVP_APIS_SUPPORTED
    &s2n_ecc_curve_x25519,
#endif
    &s2n_ecc_curve_secp384r1,
};

/*
 * These curves were chosen based on the following specification:
 * https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-52r2.pdf
 */
const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_default_fips[] = {
    &s2n_ecc_curve_secp256r1,
    &s2n_ecc_curve_secp384r1,
};

const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_20201021[] = {
    &s2n_ecc_curve_secp256r1,
    &s2n_ecc_curve_secp384r1,
    &s2n_ecc_curve_secp521r1,
};

/* Prefer x25519 over p256 for performance */
const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_20240603[] = {
#if EVP_APIS_SUPPORTED
    &s2n_ecc_curve_x25519,
#endif
    &s2n_ecc_curve_secp256r1,
    &s2n_ecc_curve_secp384r1,
    &s2n_ecc_curve_secp521r1,
};

const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_20210816[] = {
    &s2n_ecc_curve_secp384r1,
};

const struct s2n_ecc_named_curve *const s2n_ecc_pref_list_test_all[] = {
#if EVP_APIS_SUPPORTED
    &s2n_ecc_curve_x25519,
#endif
    &s2n_ecc_curve_secp256r1,
    &s2n_ecc_curve_secp384r1,
    &s2n_ecc_curve_secp521r1,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_20240501 = {
    .count = s2n_array_len(s2n_ecc_pref_list_20240501),
    .ecc_curves = s2n_ecc_pref_list_20240501,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_20140601 = {
    .count = s2n_array_len(s2n_ecc_pref_list_20140601),
    .ecc_curves = s2n_ecc_pref_list_20140601,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_20200310 = {
    .count = s2n_array_len(s2n_ecc_pref_list_20200310),
    .ecc_curves = s2n_ecc_pref_list_20200310,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_20230623 = {
    .count = s2n_array_len(s2n_ecc_pref_list_20230623),
    .ecc_curves = s2n_ecc_pref_list_20230623,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_default_fips = {
    .count = s2n_array_len(s2n_ecc_pref_list_default_fips),
    .ecc_curves = s2n_ecc_pref_list_default_fips,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_20201021 = {
    .count = s2n_array_len(s2n_ecc_pref_list_20201021),
    .ecc_curves = s2n_ecc_pref_list_20201021,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_20210816 = {
    .count = s2n_array_len(s2n_ecc_pref_list_20210816),
    .ecc_curves = s2n_ecc_pref_list_20210816,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_20240603 = {
    .count = s2n_array_len(s2n_ecc_pref_list_20240603),
    .ecc_curves = s2n_ecc_pref_list_20240603,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_test_all = {
    .count = s2n_array_len(s2n_ecc_pref_list_test_all),
    .ecc_curves = s2n_ecc_pref_list_test_all,
};

const struct s2n_ecc_preferences s2n_ecc_preferences_null = {
    .count = 0,
    .ecc_curves = NULL,
};

/* Checks if the ecc_curves present in s2n_ecc_preferences list is a subset of s2n_all_supported_curves_list
 * maintained in s2n_ecc_evp.c */
int s2n_check_ecc_preferences_curves_list(const struct s2n_ecc_preferences *ecc_preferences)
{
    int check = 1;
    for (int i = 0; i < ecc_preferences->count; i++) {
        const struct s2n_ecc_named_curve *named_curve = ecc_preferences->ecc_curves[i];
        int curve_found = 0;
        for (size_t j = 0; j < s2n_all_supported_curves_list_len; j++) {
            if (named_curve->iana_id == s2n_all_supported_curves_list[j]->iana_id) {
                curve_found = 1;
                break;
            }
        }
        check *= curve_found;
        if (check == 0) {
            POSIX_BAIL(S2N_ERR_ECDHE_UNSUPPORTED_CURVE);
        }
    }
    return S2N_SUCCESS;
}

/* Determines if query_iana_id corresponds to a curve for these ECC preferences. */
bool s2n_ecc_preferences_includes_curve(const struct s2n_ecc_preferences *ecc_preferences, uint16_t query_iana_id)
{
    if (ecc_preferences == NULL) {
        return false;
    }

    for (size_t i = 0; i < ecc_preferences->count; i++) {
        if (query_iana_id == ecc_preferences->ecc_curves[i]->iana_id) {
            return true;
        }
    }

    return false;
}
