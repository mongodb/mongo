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

#include <stdint.h>

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_tls.h"

int s2n_server_done_recv(struct s2n_connection *conn)
{
    S2N_ERROR_IF(s2n_stuffer_data_available(&conn->handshake.io), S2N_ERR_BAD_MESSAGE);

    return 0;
}

int s2n_server_done_send(struct s2n_connection *conn)
{
    return 0;
}
