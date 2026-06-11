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

#include "tls/extensions/s2n_client_pq_kem.h"

#include <stdint.h>
#include <sys/param.h>

#include "crypto/s2n_pq.h"
#include "tls/s2n_kem.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_safety.h"

static bool s2n_client_pq_kem_should_send(struct s2n_connection *conn);
static int s2n_client_pq_kem_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_pq_kem_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_pq_kem_extension = {
    .iana_value = TLS_EXTENSION_PQ_KEM_PARAMETERS,
    .is_response = false,
    .send = s2n_client_pq_kem_send,
    .recv = s2n_client_pq_kem_recv,
    .should_send = s2n_client_pq_kem_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static bool s2n_client_pq_kem_should_send(struct s2n_connection *conn)
{
    const struct s2n_security_policy *security_policy = NULL;
    return s2n_connection_get_security_policy(conn, &security_policy) == S2N_SUCCESS
            && s2n_pq_kem_is_extension_required(security_policy)
            && s2n_pq_is_enabled();
}

static int s2n_client_pq_kem_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    const struct s2n_kem_preferences *kem_preferences = NULL;
    POSIX_GUARD(s2n_connection_get_kem_preferences(conn, &kem_preferences));
    POSIX_ENSURE_REF(kem_preferences);

    POSIX_GUARD(s2n_stuffer_write_uint16(out, kem_preferences->kem_count * sizeof(kem_extension_size)));
    for (int i = 0; i < kem_preferences->kem_count; i++) {
        POSIX_GUARD(s2n_stuffer_write_uint16(out, kem_preferences->kems[i]->kem_extension_id));
    }

    return S2N_SUCCESS;
}

static int s2n_client_pq_kem_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    uint16_t size_of_all = 0;
    struct s2n_blob *proposed_kems = &conn->kex_params.client_pq_kem_extension;

    /* Ignore extension if PQ is disabled */
    if (!s2n_pq_is_enabled()) {
        return S2N_SUCCESS;
    }

    POSIX_GUARD(s2n_stuffer_read_uint16(extension, &size_of_all));
    if (size_of_all > s2n_stuffer_data_available(extension) || size_of_all % sizeof(kem_extension_size)) {
        /* Malformed length, ignore the extension */
        return S2N_SUCCESS;
    }

    proposed_kems->size = size_of_all;
    proposed_kems->data = s2n_stuffer_raw_read(extension, proposed_kems->size);
    POSIX_ENSURE_REF(proposed_kems->data);

    return S2N_SUCCESS;
}
