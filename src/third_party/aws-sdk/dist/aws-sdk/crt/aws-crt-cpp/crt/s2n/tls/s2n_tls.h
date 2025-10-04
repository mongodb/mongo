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

#include <stdbool.h>
#include <stdint.h>

#include "tls/s2n_connection.h"

extern uint8_t s2n_unknown_protocol_version;
extern uint8_t s2n_highest_protocol_version;

int s2n_flush(struct s2n_connection *conn, s2n_blocked_status *more);
S2N_RESULT s2n_client_hello_request_validate(struct s2n_connection *conn);
S2N_RESULT s2n_client_hello_request_recv(struct s2n_connection *conn);
int s2n_client_hello_send(struct s2n_connection *conn);
int s2n_client_hello_recv(struct s2n_connection *conn);
int s2n_establish_session(struct s2n_connection *conn);
int s2n_sslv2_client_hello_recv(struct s2n_connection *conn);
int s2n_server_hello_retry_send(struct s2n_connection *conn);
int s2n_server_hello_retry_recv(struct s2n_connection *conn);
int s2n_server_hello_write_message(struct s2n_connection *conn);
int s2n_server_hello_send(struct s2n_connection *conn);
int s2n_server_hello_recv(struct s2n_connection *conn);
int s2n_encrypted_extensions_send(struct s2n_connection *conn);
int s2n_encrypted_extensions_recv(struct s2n_connection *conn);
int s2n_next_protocol_send(struct s2n_connection *conn);
int s2n_next_protocol_recv(struct s2n_connection *conn);
int s2n_server_cert_send(struct s2n_connection *conn);
int s2n_server_cert_recv(struct s2n_connection *conn);
int s2n_server_status_send(struct s2n_connection *conn);
int s2n_server_status_recv(struct s2n_connection *conn);
int s2n_server_key_send(struct s2n_connection *conn);
int s2n_server_key_recv(struct s2n_connection *conn);
int s2n_cert_req_recv(struct s2n_connection *conn);
int s2n_cert_req_send(struct s2n_connection *conn);
int s2n_tls13_cert_req_send(struct s2n_connection *conn);
int s2n_tls13_cert_req_recv(struct s2n_connection *conn);
int s2n_server_done_send(struct s2n_connection *conn);
int s2n_server_done_recv(struct s2n_connection *conn);
int s2n_client_cert_recv(struct s2n_connection *conn);
int s2n_client_cert_send(struct s2n_connection *conn);
int s2n_client_key_send(struct s2n_connection *conn);
int s2n_client_key_recv(struct s2n_connection *conn);
int s2n_client_cert_verify_recv(struct s2n_connection *conn);
int s2n_client_cert_verify_send(struct s2n_connection *conn);
int s2n_tls13_cert_verify_recv(struct s2n_connection *conn);
int s2n_tls13_cert_verify_send(struct s2n_connection *conn);
int s2n_server_nst_send(struct s2n_connection *conn);
int s2n_server_nst_recv(struct s2n_connection *conn);
S2N_RESULT s2n_tls13_server_nst_send(struct s2n_connection *conn, s2n_blocked_status *blocked);
S2N_RESULT s2n_tls13_server_nst_write(struct s2n_connection *conn, struct s2n_stuffer *output);
S2N_RESULT s2n_tls13_server_nst_recv(struct s2n_connection *conn, struct s2n_stuffer *input);
int s2n_ccs_send(struct s2n_connection *conn);
int s2n_basic_ccs_recv(struct s2n_connection *conn);
int s2n_server_ccs_recv(struct s2n_connection *conn);
int s2n_client_ccs_recv(struct s2n_connection *conn);
int s2n_client_finished_send(struct s2n_connection *conn);
int s2n_client_finished_recv(struct s2n_connection *conn);
int s2n_server_finished_send(struct s2n_connection *conn);
int s2n_server_finished_recv(struct s2n_connection *conn);
int s2n_tls13_client_finished_send(struct s2n_connection *conn);
int s2n_tls13_client_finished_recv(struct s2n_connection *conn);
int s2n_tls13_server_finished_send(struct s2n_connection *conn);
int s2n_tls13_server_finished_recv(struct s2n_connection *conn);
int s2n_end_of_early_data_send(struct s2n_connection *conn);
int s2n_end_of_early_data_recv(struct s2n_connection *conn);
int s2n_process_client_hello(struct s2n_connection *conn);
int s2n_handshake_write_header(struct s2n_stuffer *out, uint8_t message_type);
int s2n_handshake_finish_header(struct s2n_stuffer *out);
S2N_RESULT s2n_handshake_parse_header(struct s2n_stuffer *io, uint8_t *message_type, uint32_t *length);
int s2n_read_full_record(struct s2n_connection *conn, uint8_t *record_type, int *isSSLv2);
S2N_RESULT s2n_sendv_with_offset_total_size(const struct iovec *bufs, ssize_t count,
        ssize_t offs, ssize_t *total_size_out);
S2N_RESULT s2n_recv_in_init(struct s2n_connection *conn, uint32_t written, uint32_t size);

extern uint16_t mfl_code_to_length[5];

#define s2n_server_received_server_name(conn) ((conn)->server_name[0] != 0)

#define s2n_server_can_send_ec_point_formats(conn) \
    ((conn)->ec_point_formats)

#define s2n_server_can_send_ocsp(conn) ((conn)->mode == S2N_SERVER \
        && (conn)->status_type == S2N_STATUS_REQUEST_OCSP          \
        && (conn)->handshake_params.our_chain_and_key              \
        && (conn)->handshake_params.our_chain_and_key->ocsp_status.size > 0)

#define s2n_server_sent_ocsp(conn) ((conn)->mode == S2N_CLIENT \
        && (conn)->status_type == S2N_STATUS_REQUEST_OCSP)

#define s2n_server_can_send_sct_list(conn) ((conn)->mode == S2N_SERVER \
        && (conn)->ct_level_requested == S2N_CT_SUPPORT_REQUEST        \
        && (conn)->handshake_params.our_chain_and_key                  \
        && (conn)->handshake_params.our_chain_and_key->sct_list.size > 0)

#define s2n_server_sending_nst(conn) ((conn)->config->use_tickets \
        && (conn)->session_ticket_status == S2N_NEW_TICKET)
