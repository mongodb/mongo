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

#include <strings.h>

#include "error/s2n_errno.h"
#include "tls/extensions/s2n_cert_status.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_config.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"
#include "tls/s2n_x509_validator.h"
#include "utils/s2n_safety.h"

int s2n_server_status_send(struct s2n_connection *conn)
{
    if (s2n_server_can_send_ocsp(conn)) {
        POSIX_GUARD(s2n_cert_status_send(conn, &conn->handshake.io));
    }

    return 0;
}

int s2n_server_status_recv(struct s2n_connection *conn)
{
    return s2n_cert_status_recv(conn, &conn->handshake.io);
}
