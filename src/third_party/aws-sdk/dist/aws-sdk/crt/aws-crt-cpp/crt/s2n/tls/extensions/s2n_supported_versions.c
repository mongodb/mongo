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

#include "tls/extensions/s2n_supported_versions.h"

#include <stdint.h>
#include <sys/param.h>

#include "tls/s2n_security_policies.h"
#include "utils/s2n_safety.h"

S2N_RESULT s2n_connection_get_minimum_supported_version(struct s2n_connection *conn, uint8_t *min_version)
{
    RESULT_ENSURE_REF(min_version);

    const struct s2n_security_policy *security_policy = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_security_policy(conn, &security_policy));
    RESULT_ENSURE_REF(security_policy);
    *min_version = security_policy->minimum_protocol_version;

    /* QUIC requires >= TLS1.3 */
    if (s2n_connection_is_quic_enabled(conn)) {
        *min_version = MAX(*min_version, S2N_TLS13);
    }

    return S2N_RESULT_OK;
}
