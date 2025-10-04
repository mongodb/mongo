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

#include <s2n.h>

#if ((__GNUC__ >= 4) || defined(__clang__)) && defined(S2N_EXPORTS)
    #define S2N_PRIVATE_API __attribute__((visibility("default")))
#else
    #define S2N_PRIVATE_API
#endif /* __GNUC__ >= 4 || defined(__clang__) */

/*
 * Internal APIs.
 *
 * These APIs change the behavior of S2N in potentially dangerous ways and should only be
 * used for testing purposes. All Internal APIs are subject to change without notice.
 */

/*
 * Gets the config set on the connection.
 *
 * This function will return a pointer to the config set by `s2n_connection_set_config`.
 * It will return NULL prior to `s2n_connection_set_config` being called and a config
 * being set by the application.
 *
 * Caution: A config can be associated with multiple connections and should not be
 * modified after it has been built. Doing so is undefined behavior.
 */
S2N_PRIVATE_API int s2n_connection_get_config(struct s2n_connection *conn, struct s2n_config **config);

/*
 * Sets a certificate chain on the config.
 *
 * It does NOT set a private key, so the connection will need to be configured to
 * [offload private key operations](https://github.com/aws/s2n-tls/blob/main/docs/usage-guide/topics/ch12-private-key-ops.md).
 */
S2N_PRIVATE_API int s2n_config_add_cert_chain(struct s2n_config *config,
        uint8_t *cert_chain_pem, uint32_t cert_chain_pem_size);

/*
 * Attempts to flush any data buffered for sending.
 *
 * This method is not sufficient to complete a previous partial send. It can only
 * attempt to flush data that has been encrypted and buffered, not data that
 * is still waiting for encryption.
 */
S2N_PRIVATE_API int s2n_flush(struct s2n_connection *conn, s2n_blocked_status *blocked);
