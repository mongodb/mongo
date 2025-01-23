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

#include <errno.h>
#include <signal.h>
#include <stdint.h>

#include "api/s2n.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_hmac.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_client_hello.h"
#include "tls/s2n_config.h"
#include "tls/s2n_crypto.h"
#include "tls/s2n_early_data.h"
#include "tls/s2n_ecc_preferences.h"
#include "tls/s2n_handshake.h"
#include "tls/s2n_kem_preferences.h"
#include "tls/s2n_key_update.h"
#include "tls/s2n_post_handshake.h"
#include "tls/s2n_prf.h"
#include "tls/s2n_quic_support.h"
#include "tls/s2n_record.h"
#include "tls/s2n_resume.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_tls_parameters.h"
#include "tls/s2n_x509_validator.h"
#include "utils/s2n_atomic.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_timer.h"

#define S2N_TLS_PROTOCOL_VERSION_LEN 2

#define S2N_PEER_MODE(our_mode) ((our_mode + 1) % 2)

#define is_handshake_complete(conn) (APPLICATION_DATA == s2n_conn_get_current_message_type(conn))

#define S2N_DEFAULT_BLINDING_MAX 30
#define S2N_DEFAULT_BLINDING_MIN 10

typedef enum {
    S2N_NO_TICKET = 0,
    S2N_DECRYPT_TICKET,
    S2N_NEW_TICKET
} s2n_session_ticket_status;

struct s2n_connection {
    /* Is this connection using CORK/SO_RCVLOWAT optimizations? Only valid when the connection is using
     * managed_send_io
     */
    unsigned corked_io : 1;

    /* Session resumption indicator on client side */
    unsigned client_session_resumed : 1;

    /* Connection can be used by a QUIC implementation */
    unsigned quic_enabled : 1;

    /* RFC5746 Section 4.3 suggests servers implement a minimal version of the
     * renegotiation_info extension even if renegotiation is not supported.
     * Some clients may fail the handshake if a corresponding renegotiation_info
     * extension is not sent back by the server.
     */
    unsigned secure_renegotiation : 1;
    /* Was the EC point formats sent by the client */
    unsigned ec_point_formats : 1;

    /* whether the connection address is ipv6 or not */
    unsigned ipv6 : 1;

    /* Whether server_name extension was used to make a decision on cert selection.
     * RFC6066 Section 3 states that server which used server_name to make a decision
     * on certificate or security settings has to send an empty server_name.
     */
    unsigned server_name_used : 1;

    /* If write fd is broken */
    unsigned write_fd_broken : 1;

    /* Has the user set their own I/O callbacks or is this connection using the
     * default socket-based I/O set by s2n */
    unsigned managed_send_io : 1;
    unsigned managed_recv_io : 1;

    /* Early data supported by caller.
     * If a caller does not use any APIs that support early data,
     * do not negotiate early data.
     */
    unsigned early_data_expected : 1;

    /* Connection overrides server_max_early_data_size */
    unsigned server_max_early_data_size_overridden : 1;

    /* Connection overrides psk_mode.
     * This means that the connection will keep the existing value of psk_params->type,
     * even when setting a new config. */
    unsigned psk_mode_overridden : 1;

    /* Connection negotiated an EMS */
    unsigned ems_negotiated : 1;

    /* Connection successfully set a ticket on the connection */
    unsigned set_session : 1;

    /* Buffer multiple records before flushing them.
     * This allows multiple records to be written with one socket send. */
    unsigned multirecord_send : 1;

    /* If enabled, this connection will free each of its IO buffers after all data
     * has been flushed */
    unsigned dynamic_buffers : 1;

    /* Indicates protocol negotiation will be done through the NPN extension
     * instead of the ALPN extension */
    unsigned npn_negotiated : 1;

    /* Marks if kTLS has been enabled for this connection. */
    unsigned ktls_send_enabled : 1;
    unsigned ktls_recv_enabled : 1;

    /* Indicates whether the connection should request OCSP stapling from the peer */
    unsigned request_ocsp_status : 1;

    /* Indicates that the connection was created from deserialization
     * and therefore knowledge of the original handshake is limited. */
    unsigned deserialized_conn : 1;

    /* Indicates s2n_recv should reduce read calls by attempting to buffer more
     * data than is required for a single record.
     *
     * This is more efficient, but will break applications that expect exact reads,
     * for example any custom IO that behaves like MSG_WAITALL.
     */
    unsigned recv_buffering : 1;

    /* The configuration (cert, key .. etc ) */
    struct s2n_config *config;

    /* Overrides Security Policy in config if non-null */
    const struct s2n_security_policy *security_policy_override;

    /* The user defined context associated with connection */
    void *context;

    /* The user defined secret callback and context */
    s2n_secret_cb secret_cb;
    void *secret_cb_context;

    /* The send and receive callbacks don't have to be the same (e.g. two pipes) */
    s2n_send_fn *send;
    s2n_recv_fn *recv;

    /* The context passed to the I/O callbacks */
    void *send_io_context;
    void *recv_io_context;

    /* Track request/response extensions to ensure correct response extension behavior.
     *
     * We need to track client and server extensions separately because some
     * extensions (like request_status and other Certificate extensions) can
     * be requested by the client, the server, or both.
     */
    s2n_extension_bitfield extension_requests_sent;
    s2n_extension_bitfield extension_requests_received;
    s2n_extension_bitfield extension_responses_received;

    /* Is this connection a client or a server connection */
    s2n_mode mode;

    /* Does s2n handle the blinding, or does the application */
    s2n_blinding blinding;

    /* A timer to measure the time between record writes */
    struct s2n_timer write_timer;

    /* last written time */
    uint64_t last_write_elapsed;

    /* When fatal errors occurs, s2n imposes a pause before
     * the connection is closed. If non-zero, this value tracks
     * how many nanoseconds to pause - which will be relative to
     * the write_timer value. */
    uint64_t delay;

    /* The session id */
    uint8_t session_id[S2N_TLS_SESSION_ID_MAX_LEN];
    uint8_t session_id_len;

    /* The version advertised by the client, by the
     * server, and the actual version we are currently
     * speaking. */
    uint8_t client_hello_version;
    uint8_t client_protocol_version;
    uint8_t server_protocol_version;
    uint8_t actual_protocol_version;
    /* The version stored in the ticket / session we are resuming.
     * We expect the connection to negotiate this version during
     * the resumption handshake.
     */
    uint8_t resume_protocol_version;

    /* Flag indicating whether a protocol version has been
     * negotiated yet. */
    uint8_t actual_protocol_version_established;

    /* Our crypto parameters */
    struct s2n_crypto_parameters *initial;
    struct s2n_crypto_parameters *secure;
    struct s2n_secrets secrets;

    /* Which set is the client/server actually using? */
    struct s2n_crypto_parameters *client;
    struct s2n_crypto_parameters *server;

    /* Contains parameters needed to negotiate a shared secret */
    struct s2n_kex_parameters kex_params;

    /* Contains parameters needed during the handshake phase */
    struct s2n_handshake_parameters handshake_params;

    /* Our PSK parameters */
    struct s2n_psk_parameters psk_params;

    /* The PRF needs some storage elements to work with */
    struct s2n_prf_working_space *prf_space;

    /* Indicates whether the application has overridden the client auth behavior
     * inherited from the config.
     * This should be a bitflag, but that change is blocked on the SAW proofs.
     */
    uint8_t client_cert_auth_type_overridden;

    /* Whether or not the client should authenticate itself to the server.
     * Only used if client_cert_auth_type_overridden is true.
     */
    s2n_cert_auth_type client_cert_auth_type;

    /* Our workhorse stuffers, used for buffering the plaintext
     * and encrypted data in both directions.
     */
    uint8_t header_in_data[S2N_TLS_RECORD_HEADER_LENGTH];
    struct s2n_stuffer header_in;
    struct s2n_stuffer buffer_in;
    struct s2n_stuffer in;
    struct s2n_stuffer out;
    enum {
        ENCRYPTED,
        PLAINTEXT
    } in_status;

    /* How much of the current user buffer have we already
     * encrypted and sent or have pending for the wire but have
     * not acknowledged to the user.
     */
    ssize_t current_user_data_consumed;

    /* An alert may be fragmented across multiple records,
     * this stuffer is used to re-assemble.
     */
    uint8_t alert_in_data[S2N_ALERT_LENGTH];
    struct s2n_stuffer alert_in;

    /* Both readers and writers can trigger alerts.
     * We prioritize writer alerts over reader alerts.
     */
    uint8_t writer_alert_out;
    uint8_t reader_alert_out;
    uint8_t reader_warning_out;
    bool alert_sent;

    /* Receiving error or close_notify alerts changes the behavior of s2n_shutdown_send */
    s2n_atomic_flag error_alert_received;
    s2n_atomic_flag close_notify_received;

    /* Our handshake state machine */
    struct s2n_handshake handshake;

    /* Maximum outgoing fragment size for this connection. Does not limit
     * incoming record size.
     *
     * This value is updated when:
     *   1. s2n_connection_prefer_low_latency is set
     *   2. s2n_connection_prefer_throughput is set
     *   3. TLS Maximum Fragment Length extension is negotiated
     *
     * Default value: S2N_DEFAULT_FRAGMENT_LENGTH
     */
    uint16_t max_outgoing_fragment_length;

    /* The number of bytes to send before changing the record size.
     * If this value > 0 then dynamic TLS record size is enabled. Otherwise, the feature is disabled (default).
     */
    uint32_t dynamic_record_resize_threshold;

    /* Reset record size back to a single segment after threshold seconds of inactivity */
    uint16_t dynamic_record_timeout_threshold;

    /* The number of bytes consumed during a period of application activity.
     * Used for dynamic record sizing. */
    uint64_t active_application_bytes_consumed;

    /* Negotiated TLS extension Maximum Fragment Length code.
     * If set, the client and server have both agreed to fragment their records to the given length. */
    uint8_t negotiated_mfl_code;

    /* Keep some accounting on each connection */
    uint64_t wire_bytes_in;
    uint64_t wire_bytes_out;
    uint64_t early_data_bytes;

    /* Either the reader or the writer can trigger both sides of the connection
     * to close in response to a fatal error.
     */
    s2n_atomic_flag read_closed;
    s2n_atomic_flag write_closed;

    /* TLS extension data */
    char server_name[S2N_MAX_SERVER_NAME + 1];

    /* The application protocol decided upon during the client hello.
     * If ALPN is being used, then:
     * In server mode, this will be set by the time client_hello_cb is invoked.
     * In client mode, this will be set after is_handshake_complete(connection) is true.
     */
    char application_protocol[256];

    /* OCSP stapling response data */
    s2n_status_request_type status_type;
    struct s2n_blob status_response;

    /* Certificate Transparency response data */
    s2n_ct_support_level ct_level_requested;
    struct s2n_blob ct_response;

    /* QUIC transport parameters data: https://tools.ietf.org/html/draft-ietf-quic-tls-29#section-8.2 */
    struct s2n_blob our_quic_transport_parameters;
    struct s2n_blob peer_quic_transport_parameters;

    struct s2n_client_hello client_hello;

    struct s2n_x509_validator x509_validator;

    /* After a connection is created this is the verification function that should always be used. At init time,
     * the config should be checked for a verify callback and each connection should default to that. However,
     * from the user's perspective, it's sometimes simpler to manage state by attaching each validation function/data
     * to the connection, instead of globally to a single config.*/
    s2n_verify_host_fn verify_host_fn;
    void *data_for_verify_host;
    uint8_t verify_host_fn_overridden;

    /* Session ticket data */
    s2n_session_ticket_status session_ticket_status;
    struct s2n_blob client_ticket;
    uint32_t ticket_lifetime_hint;
    struct s2n_ticket_fields tls13_ticket_fields;

    /* Session ticket extension from client to attempt to decrypt as the server. */
    uint8_t ticket_ext_data[S2N_TLS12_TICKET_SIZE_IN_BYTES];
    struct s2n_stuffer client_ticket_to_decrypt;

    /* application protocols overridden */
    struct s2n_blob application_protocols_overridden;

    /* Cookie extension data */
    struct s2n_blob cookie;

    /* Flags to prevent users from calling methods recursively.
     * This can be an easy mistake to make when implementing callbacks.
     */
    bool send_in_use;
    bool recv_in_use;
    bool negotiate_in_use;

    uint16_t tickets_to_send;
    uint16_t tickets_sent;

    s2n_early_data_state early_data_state;
    uint32_t server_max_early_data_size;
    struct s2n_blob server_early_data_context;
    uint32_t server_keying_material_lifetime;

    struct s2n_post_handshake post_handshake;
    /* Both the reader and writer can set key_update_pending.
     * The writer clears it after a KeyUpdate is sent.
     */
    s2n_atomic_flag key_update_pending;

    /* Track KeyUpdates for metrics */
    uint8_t send_key_updated;
    uint8_t recv_key_updated;
};

S2N_CLEANUP_RESULT s2n_connection_ptr_free(struct s2n_connection **s2n_connection);

int s2n_connection_is_managed_corked(const struct s2n_connection *s2n_connection);
int s2n_connection_is_client_auth_enabled(struct s2n_connection *s2n_connection);

typedef enum {
    S2N_IO_WRITABLE,
    S2N_IO_READABLE,
    S2N_IO_FULL_DUPLEX,
    S2N_IO_CLOSED,
} s2n_io_status;
bool s2n_connection_check_io_status(struct s2n_connection *conn, s2n_io_status status);
S2N_RESULT s2n_connection_set_closed(struct s2n_connection *conn);

/* Send/recv a stuffer to/from a connection */
int s2n_connection_send_stuffer(struct s2n_stuffer *stuffer, struct s2n_connection *conn, uint32_t len);
int s2n_connection_recv_stuffer(struct s2n_stuffer *stuffer, struct s2n_connection *conn, uint32_t len);

S2N_RESULT s2n_connection_wipe_all_keyshares(struct s2n_connection *conn);

/* If dynamic buffers are enabled, the IO buffers may be freed if they are completely consumed */
S2N_RESULT s2n_connection_dynamic_free_in_buffer(struct s2n_connection *conn);
S2N_RESULT s2n_connection_dynamic_free_out_buffer(struct s2n_connection *conn);

int s2n_connection_get_cipher_preferences(struct s2n_connection *conn, const struct s2n_cipher_preferences **cipher_preferences);
int s2n_connection_get_security_policy(struct s2n_connection *conn, const struct s2n_security_policy **security_policy);
int s2n_connection_get_kem_preferences(struct s2n_connection *conn, const struct s2n_kem_preferences **kem_preferences);
int s2n_connection_get_signature_preferences(struct s2n_connection *conn, const struct s2n_signature_preferences **signature_preferences);
int s2n_connection_get_ecc_preferences(struct s2n_connection *conn, const struct s2n_ecc_preferences **ecc_preferences);
int s2n_connection_get_protocol_preferences(struct s2n_connection *conn, struct s2n_blob **protocol_preferences);
int s2n_connection_set_client_auth_type(struct s2n_connection *conn, s2n_cert_auth_type cert_auth_type);
int s2n_connection_get_client_auth_type(struct s2n_connection *conn, s2n_cert_auth_type *client_cert_auth_type);
int s2n_connection_get_client_cert_chain(struct s2n_connection *conn, uint8_t **der_cert_chain_out, uint32_t *cert_chain_len);
int s2n_connection_get_peer_cert_chain(const struct s2n_connection *conn, struct s2n_cert_chain_and_key *cert_chain_and_key);
uint8_t s2n_connection_get_protocol_version(const struct s2n_connection *conn);
S2N_RESULT s2n_connection_set_max_fragment_length(struct s2n_connection *conn, uint16_t length);
S2N_RESULT s2n_connection_get_secure_cipher(struct s2n_connection *conn, const struct s2n_cipher **cipher);
S2N_RESULT s2n_connection_get_sequence_number(struct s2n_connection *conn,
        s2n_mode mode, struct s2n_blob *seq_num);
