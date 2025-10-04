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

#include "tls/extensions/s2n_extension_type_lists.h"

#include "api/s2n.h"
#include "tls/extensions/s2n_cert_authorities.h"
#include "tls/extensions/s2n_cert_status.h"
#include "tls/extensions/s2n_cert_status_response.h"
#include "tls/extensions/s2n_client_alpn.h"
#include "tls/extensions/s2n_client_cert_status_request.h"
#include "tls/extensions/s2n_client_key_share.h"
#include "tls/extensions/s2n_client_max_frag_len.h"
#include "tls/extensions/s2n_client_pq_kem.h"
#include "tls/extensions/s2n_client_psk.h"
#include "tls/extensions/s2n_client_renegotiation_info.h"
#include "tls/extensions/s2n_client_sct_list.h"
#include "tls/extensions/s2n_client_server_name.h"
#include "tls/extensions/s2n_client_session_ticket.h"
#include "tls/extensions/s2n_client_signature_algorithms.h"
#include "tls/extensions/s2n_client_supported_groups.h"
#include "tls/extensions/s2n_client_supported_versions.h"
#include "tls/extensions/s2n_cookie.h"
#include "tls/extensions/s2n_early_data_indication.h"
#include "tls/extensions/s2n_ec_point_format.h"
#include "tls/extensions/s2n_ems.h"
#include "tls/extensions/s2n_npn.h"
#include "tls/extensions/s2n_psk_key_exchange_modes.h"
#include "tls/extensions/s2n_quic_transport_params.h"
#include "tls/extensions/s2n_server_alpn.h"
#include "tls/extensions/s2n_server_cert_status_request.h"
#include "tls/extensions/s2n_server_key_share.h"
#include "tls/extensions/s2n_server_max_fragment_length.h"
#include "tls/extensions/s2n_server_psk.h"
#include "tls/extensions/s2n_server_renegotiation_info.h"
#include "tls/extensions/s2n_server_sct_list.h"
#include "tls/extensions/s2n_server_server_name.h"
#include "tls/extensions/s2n_server_session_ticket.h"
#include "tls/extensions/s2n_server_signature_algorithms.h"
#include "tls/extensions/s2n_server_supported_versions.h"
#include "tls/s2n_connection.h"

static const s2n_extension_type *const client_hello_extensions[] = {
    &s2n_client_supported_versions_extension,

    /* We MUST process key_share after supported_groups,
     * because we need to choose the keyshare based on the
     * mutually supported groups. */
    &s2n_client_supported_groups_extension,
    &s2n_client_key_share_extension,

    &s2n_client_signature_algorithms_extension,
    &s2n_client_server_name_extension,

    /* We MUST process the NPN extension after the ALPN extension
     * because NPN is only negotiated if ALPN is not */
    &s2n_client_alpn_extension,
    &s2n_client_npn_extension,

    &s2n_client_cert_status_request_extension,
    &s2n_client_sct_list_extension,
    &s2n_client_max_frag_len_extension,
    &s2n_client_session_ticket_extension,
    &s2n_client_ec_point_format_extension,
    &s2n_client_pq_kem_extension,
    &s2n_client_renegotiation_info_extension,
    &s2n_client_cookie_extension,
    &s2n_quic_transport_parameters_extension,
    &s2n_psk_key_exchange_modes_extension,
    &s2n_client_early_data_indication_extension,
    &s2n_client_ems_extension,
    &s2n_client_psk_extension /* MUST be last */
};

static const s2n_extension_type *const tls12_server_hello_extensions[] = {
    &s2n_server_supported_versions_extension,
    &s2n_server_server_name_extension,
    &s2n_server_ec_point_format_extension,
    &s2n_server_renegotiation_info_extension,
    &s2n_server_alpn_extension,
    &s2n_cert_status_response_extension,
    &s2n_server_sct_list_extension,
    &s2n_server_max_fragment_length_extension,
    &s2n_server_session_ticket_extension,
    &s2n_server_ems_extension,
    &s2n_server_npn_extension,
};

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.1.4
 *# The
 *# HelloRetryRequest extensions defined in this specification are:
 *#
 *# -  supported_versions (see Section 4.2.1)
 *#
 *# -  cookie (see Section 4.2.2)
 *#
 *# -  key_share (see Section 4.2.8)
 */
static const s2n_extension_type *const hello_retry_request_extensions[] = {
    &s2n_server_supported_versions_extension,
    &s2n_server_cookie_extension,
    &s2n_server_key_share_extension,
};

static const s2n_extension_type *const tls13_server_hello_extensions[] = {
    &s2n_server_supported_versions_extension,
    &s2n_server_key_share_extension,
    &s2n_server_psk_extension, /* MUST appear after keyshare extension */
};

static const s2n_extension_type *const encrypted_extensions[] = {
    &s2n_server_server_name_extension,
    &s2n_server_max_fragment_length_extension,
    &s2n_server_alpn_extension,
    &s2n_quic_transport_parameters_extension,
    &s2n_server_early_data_indication_extension,
};

static const s2n_extension_type *const cert_req_extensions[] = {
    &s2n_server_signature_algorithms_extension,
    &s2n_server_cert_status_request_extension,
    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.4
     *= type=exception
     *= reason=Currently only supported for servers -- no client use case
     *# The client MAY send the "certificate_authorities" extension in the
     *# ClientHello message.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.4
     *# The server MAY send it in the CertificateRequest message.
     */
    &s2n_cert_authorities_extension,
};

static const s2n_extension_type *const certificate_extensions[] = {
    &s2n_cert_status_extension,
    &s2n_server_sct_list_extension,
};

static const s2n_extension_type *const nst_extensions[] = {
    &s2n_nst_early_data_indication_extension,
};

#define S2N_EXTENSION_LIST(list)                                \
    {                                                           \
        .extension_types = (list), .count = s2n_array_len(list) \
    }

static s2n_extension_type_list extension_lists[] = {
    [S2N_EXTENSION_LIST_CLIENT_HELLO] = S2N_EXTENSION_LIST(client_hello_extensions),
    [S2N_EXTENSION_LIST_HELLO_RETRY_REQUEST] = S2N_EXTENSION_LIST(hello_retry_request_extensions),
    [S2N_EXTENSION_LIST_SERVER_HELLO_DEFAULT] = S2N_EXTENSION_LIST(tls12_server_hello_extensions),
    [S2N_EXTENSION_LIST_SERVER_HELLO_TLS13] = S2N_EXTENSION_LIST(tls13_server_hello_extensions),
    [S2N_EXTENSION_LIST_ENCRYPTED_EXTENSIONS] = S2N_EXTENSION_LIST(encrypted_extensions),
    [S2N_EXTENSION_LIST_CERT_REQ] = S2N_EXTENSION_LIST(cert_req_extensions),
    [S2N_EXTENSION_LIST_CERTIFICATE] = S2N_EXTENSION_LIST(certificate_extensions),
    [S2N_EXTENSION_LIST_NST] = S2N_EXTENSION_LIST(nst_extensions),
    [S2N_EXTENSION_LIST_EMPTY] = { .extension_types = NULL, .count = 0 },
};

int s2n_extension_type_list_get(s2n_extension_list_id list_type, s2n_extension_type_list **extension_list)
{
    POSIX_ENSURE_REF(extension_list);
    POSIX_ENSURE_LT(list_type, s2n_array_len(extension_lists));

    *extension_list = &extension_lists[list_type];
    return S2N_SUCCESS;
}
