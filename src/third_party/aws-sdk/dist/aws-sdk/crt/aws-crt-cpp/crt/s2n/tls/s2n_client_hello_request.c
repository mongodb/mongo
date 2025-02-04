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

#include "api/s2n.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_safety.h"

S2N_RESULT s2n_client_hello_request_validate(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    if (IS_NEGOTIATED(conn)) {
        RESULT_ENSURE(conn->actual_protocol_version < S2N_TLS13, S2N_ERR_BAD_MESSAGE);
    }

    /*
     *= https://www.rfc-editor.org/rfc/rfc5246#section-7.4.1.1
     *# The HelloRequest message MAY be sent by the server at any time.
     */
    RESULT_ENSURE(conn->mode == S2N_CLIENT, S2N_ERR_BAD_MESSAGE);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_client_hello_request_recv(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);
    RESULT_GUARD(s2n_client_hello_request_validate(conn));

    /* Maintain the old s2n-tls behavior by default.
     * Traditionally, s2n-tls has just ignored all hello requests.
     */
    if (!conn->config->renegotiate_request_cb) {
        return S2N_RESULT_OK;
    }

    /*
     *= https://www.rfc-editor.org/rfc/rfc5746#section-4.2
     *# This text applies if the connection's "secure_renegotiation" flag is
     *# set to FALSE.
     *#
     *# It is possible that un-upgraded servers will request that the client
     *# renegotiate.  It is RECOMMENDED that clients refuse this
     *# renegotiation request.  Clients that do so MUST respond to such
     *# requests with a "no_renegotiation" alert (RFC 5246 requires this
     *# alert to be at the "warning" level).  It is possible that the
     *# apparently un-upgraded server is in fact an attacker who is then
     *# allowing the client to renegotiate with a different, legitimate,
     *# upgraded server.
     */
    if (!conn->secure_renegotiation) {
        RESULT_GUARD(s2n_queue_reader_no_renegotiation_alert(conn));
        return S2N_RESULT_OK;
    }

    s2n_renegotiate_response response = S2N_RENEGOTIATE_REJECT;
    int result = conn->config->renegotiate_request_cb(conn, conn->config->renegotiate_request_ctx, &response);
    RESULT_ENSURE(result == S2N_SUCCESS, S2N_ERR_CANCELLED);

    /*
     *= https://www.rfc-editor.org/rfc/rfc5246#section-7.4.1.1
     *# This message MAY be ignored by
     *# the client if it does not wish to renegotiate a session, or the
     *# client may, if it wishes, respond with a no_renegotiation alert.
     */
    if (response == S2N_RENEGOTIATE_REJECT) {
        RESULT_GUARD(s2n_queue_reader_no_renegotiation_alert(conn));
        return S2N_RESULT_OK;
    }

    return S2N_RESULT_OK;
}
