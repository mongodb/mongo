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

#include "tls/extensions/s2n_server_cert_status_request.h"

#include "tls/s2n_connection.h"

static bool s2n_server_cert_status_request_should_send(struct s2n_connection *conn);
static int s2n_server_cert_status_request_send(struct s2n_connection *conn, struct s2n_stuffer *out);

const s2n_extension_type s2n_server_cert_status_request_extension = {
    .iana_value = TLS_EXTENSION_STATUS_REQUEST,
    .is_response = false,
    .send = s2n_server_cert_status_request_send,
    .recv = s2n_extension_recv_noop,
    .should_send = s2n_server_cert_status_request_should_send,
    .if_missing = s2n_extension_noop_if_missing,
};

static int s2n_server_cert_status_request_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#4.4.2.1
     *# A server MAY request that a client present an OCSP response with its
     *# certificate by sending an empty "status_request" extension in its
     *# CertificateRequest message.
     */
    return S2N_SUCCESS;
}

static bool s2n_server_cert_status_request_should_send(struct s2n_connection *conn)
{
    return conn->request_ocsp_status;
}
