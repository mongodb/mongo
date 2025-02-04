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

#include "tls/extensions/s2n_key_share.h"

#include "tls/s2n_tls.h"
#include "utils/s2n_safety.h"

/* Generate and write an ecc point.
 * This is used to write the ecc portion of PQ hybrid keyshares, which does NOT include the curve id.
 */
S2N_RESULT s2n_ecdhe_send_public_key(struct s2n_ecc_evp_params *ecc_evp_params, struct s2n_stuffer *out, bool len_prefixed)
{
    RESULT_ENSURE_REF(ecc_evp_params);
    RESULT_ENSURE_REF(ecc_evp_params->negotiated_curve);

    if (len_prefixed) {
        RESULT_GUARD_POSIX(s2n_stuffer_write_uint16(out, ecc_evp_params->negotiated_curve->share_size));
    }

    if (ecc_evp_params->evp_pkey == NULL) {
        RESULT_GUARD_POSIX(s2n_ecc_evp_generate_ephemeral_key(ecc_evp_params));
    }
    RESULT_GUARD_POSIX(s2n_ecc_evp_write_params_point(ecc_evp_params, out));

    return S2N_RESULT_OK;
}

/* Generate and write an ecc point and its corresponding curve id.
 * This is used to write ecc keyshares for the client and server key_share extensions.
 */
int s2n_ecdhe_parameters_send(struct s2n_ecc_evp_params *ecc_evp_params, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(ecc_evp_params);
    POSIX_ENSURE_REF(ecc_evp_params->negotiated_curve);

    POSIX_GUARD(s2n_stuffer_write_uint16(out, ecc_evp_params->negotiated_curve->iana_id));
    POSIX_GUARD_RESULT(s2n_ecdhe_send_public_key(ecc_evp_params, out, true));

    return S2N_SUCCESS;
}
