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

#include <stdint.h>

#include "api/s2n.h"
#include "crypto/s2n_certificate.h"
#include "crypto/s2n_hash.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_crypto.h"
#include "tls/s2n_handshake_hashes.h"
#include "tls/s2n_handshake_type.h"
#include "tls/s2n_signature_algorithms.h"
#include "tls/s2n_tls_parameters.h"

/* From RFC 8446: https://tools.ietf.org/html/rfc8446#appendix-B.3 */
#define TLS_HELLO_REQUEST             0
#define TLS_CLIENT_HELLO              1
#define TLS_SERVER_HELLO              2
#define TLS_SERVER_NEW_SESSION_TICKET 4
#define TLS_END_OF_EARLY_DATA         5
#define TLS_ENCRYPTED_EXTENSIONS      8
#define TLS_CERTIFICATE               11
#define TLS_SERVER_KEY                12
#define TLS_CERT_REQ                  13
#define TLS_SERVER_HELLO_DONE         14
#define TLS_CERT_VERIFY               15
#define TLS_CLIENT_KEY                16
#define TLS_FINISHED                  20
#define TLS_SERVER_CERT_STATUS        22
#define TLS_SERVER_SESSION_LOOKUP     23
#define TLS_KEY_UPDATE                24
#define TLS_NPN                       67
#define TLS_MESSAGE_HASH              254

/* Maximum number of messages in a handshake */
#define S2N_MAX_HANDSHAKE_LENGTH 32

/* This is the list of message types that we support */
typedef enum {
    CLIENT_HELLO = 0,
    SERVER_HELLO,
    SERVER_CERT,
    SERVER_NEW_SESSION_TICKET,
    SERVER_CERT_STATUS,
    SERVER_KEY,
    SERVER_CERT_REQ,
    SERVER_HELLO_DONE,
    CLIENT_CERT,
    CLIENT_KEY,
    CLIENT_CERT_VERIFY,
    CLIENT_CHANGE_CIPHER_SPEC,
    /* Not a standardized message. Defined: https://datatracker.ietf.org/doc/html/draft-agl-tls-nextprotoneg-04 */
    CLIENT_NPN,
    CLIENT_FINISHED,
    SERVER_CHANGE_CIPHER_SPEC,
    SERVER_FINISHED,

    /* TLS1.3 message types. Defined: https://tools.ietf.org/html/rfc8446#appendix-B.3 */
    ENCRYPTED_EXTENSIONS,
    SERVER_CERT_VERIFY,
    HELLO_RETRY_MSG,
    END_OF_EARLY_DATA,

    APPLICATION_DATA,
} message_type_t;

typedef enum {
    S2N_ASYNC_NOT_INVOKED = 0,
    S2N_ASYNC_INVOKED,
    S2N_ASYNC_COMPLETE,
} s2n_async_state;

/* Indicates which state machine is being used. The handshake
 * starts off on the initial enum, which indicates we're using
 * the TLS12 state machine. Once the handshake version is determined
 * the enum is set to either the TLS12 or TLS13 state machine.
 * This works because the initial entries in both the TLS12 and 
 * TLS13 state machines are the same. */
typedef enum {
    S2N_STATE_MACHINE_INITIAL = 0,
    S2N_STATE_MACHINE_TLS12,
    S2N_STATE_MACHINE_TLS13,
} s2n_state_machine;

struct s2n_handshake_parameters {
    /* Public keys for server / client */
    struct s2n_pkey server_public_key;
    struct s2n_pkey client_public_key;
    struct s2n_blob client_cert_chain;
    s2n_pkey_type client_cert_pkey_type;

    /* Signature/hash algorithm pairs offered by the peer.
     *
     * In the case of server connections, this list contains the client's supported signature
     * schemes offered in the ClientHello. In the case of client connections, this list contains
     * the server's supported signature schemes offered in the CertificateRequest.
     */
    struct s2n_sig_scheme_list peer_sig_scheme_list;
    /* Signature scheme chosen by the server */
    const struct s2n_signature_scheme *server_cert_sig_scheme;
    /* Signature scheme chosen by the client */
    const struct s2n_signature_scheme *client_cert_sig_scheme;

    /* The cert chain we will send the peer. */
    struct s2n_cert_chain_and_key *our_chain_and_key;

    /* The subset of certificates that match the server_name presented in the ClientHello.
     * In the case of multiple certificates matching a server_name, s2n will prefer certificates
     * in FIFO order based on calls to s2n_config_add_cert_chain_and_key_to_store
     *
     * Note that in addition to domain matching, the key type for the certificate must also be
     * suitable for a negotiation in order to be selected. The set of matching certs here are indexed
     * by s2n_authentication_method.
     *
     * Example:
     *    - Assume certA is added to s2n_config via s2n_config_add_cert_chain_and_key_to_store
     *    - Next certB is added.
     *    - if certA matches www.foo.com and certB matches www.foo.com, s2n will prefer certA
     *
     * Note that in addition to domain matching, the key type for the certificate must also be
     * suitable for a negotiation in order to be selected.
     *
     * Example:
     *    - Assume certA and certB match server_name www.foo.com
     *    - certA is ECDSA and certB is RSA.
     *    - Client only supports RSA ciphers
     *    - certB will be selected.
     */
    struct s2n_cert_chain_and_key *exact_sni_matches[S2N_CERT_TYPE_COUNT];
    struct s2n_cert_chain_and_key *wc_sni_matches[S2N_CERT_TYPE_COUNT];
    uint8_t exact_sni_match_exists;
    uint8_t wc_sni_match_exists;

    uint8_t client_random[S2N_TLS_RANDOM_DATA_LEN];
    uint8_t server_random[S2N_TLS_RANDOM_DATA_LEN];
};

struct s2n_handshake {
    struct s2n_stuffer io;

    struct s2n_handshake_hashes *hashes;

    /* Hash algorithms required for this handshake. The set of required hashes can be reduced as session parameters are
     * negotiated, i.e. cipher suite and protocol version.
     */
    uint8_t required_hash_algs[S2N_HASH_SENTINEL];

    /*
     * Data required by the Finished messages.
     * In TLS1.2 and earlier, the data is the verify_data.
     * In TLS1.3, the data is the finished_key used to calculate the verify_data.
     *
     * The data will be different for the client and server.
     * The length of the data will be the same for the client and server.
     * The length of the data depends on protocol version and cipher suite.
     */
    uint8_t server_finished[S2N_TLS_SECRET_LEN];
    uint8_t client_finished[S2N_TLS_SECRET_LEN];
    uint8_t finished_len;

    /* Which message-order affecting features are enabled */
    uint32_t handshake_type;

    /* Which handshake message number are we processing */
    int message_number;

    /* Last message in the handshake. Unless using early data or testing,
     * should always be APPLICATION_DATA. */
    message_type_t end_of_messages;

    /* State of the async pkey operation during handshake */
    s2n_async_state async_state;

    /* State of the async early data callback.
     * If not initialized, then the callback has not been triggered yet. */
    struct s2n_offered_early_data early_data_async_state;

    /* Indicates the CLIENT_HELLO message has been completely received */
    unsigned client_hello_received : 1;

    /* Indicates the handshake blocked while trying to read or write data, and has been paused */
    unsigned paused : 1;

    /* Set to 1 if the RSA verification failed */
    unsigned rsa_failed : 1;

    /* Indicates that this is a renegotiation handshake */
    unsigned renegotiation : 1;

    s2n_state_machine state_machine;
};

/* Only used in our test cases. */
message_type_t s2n_conn_get_current_message_type(const struct s2n_connection *conn);

/* s2n_handshake */
int s2n_handshake_require_all_hashes(struct s2n_handshake *handshake);
uint8_t s2n_handshake_is_hash_required(struct s2n_handshake *handshake, s2n_hash_algorithm hash_alg);
int s2n_conn_update_required_handshake_hashes(struct s2n_connection *conn);
S2N_RESULT s2n_handshake_copy_hash_state(struct s2n_connection *conn, s2n_hash_algorithm hash_alg, struct s2n_hash_state *hash_state);
S2N_RESULT s2n_handshake_reset_hash_state(struct s2n_connection *conn, s2n_hash_algorithm hash_alg);
int s2n_conn_find_name_matching_certs(struct s2n_connection *conn);
int s2n_create_wildcard_hostname(struct s2n_stuffer *hostname, struct s2n_stuffer *output);
struct s2n_cert_chain_and_key *s2n_get_compatible_cert_chain_and_key(struct s2n_connection *conn, const s2n_pkey_type cert_type);
S2N_RESULT s2n_negotiate_until_message(struct s2n_connection *conn, s2n_blocked_status *blocked, message_type_t end_message);
S2N_RESULT s2n_handshake_validate(const struct s2n_handshake *s2n_handshake);
S2N_RESULT s2n_handshake_set_finished_len(struct s2n_connection *conn, uint8_t len);
bool s2n_handshake_is_renegotiation(struct s2n_connection *conn);
S2N_RESULT s2n_handshake_message_send(struct s2n_connection *conn, uint8_t content_type, s2n_blocked_status *blocked);

/* s2n_handshake_io */
int s2n_conn_set_handshake_type(struct s2n_connection *conn);
int s2n_conn_set_handshake_no_client_cert(struct s2n_connection *conn);
S2N_RESULT s2n_conn_choose_state_machine(struct s2n_connection *conn, uint8_t protocol_version);
bool s2n_handshake_is_complete(struct s2n_connection *conn);

/* s2n_handshake_transcript */
S2N_RESULT s2n_handshake_transcript_update(struct s2n_connection *conn);
int s2n_conn_update_handshake_hashes(struct s2n_connection *conn, struct s2n_blob *data);

/* s2n_quic_support */
S2N_RESULT s2n_quic_read_handshake_message(struct s2n_connection *conn, uint8_t *message_type);
S2N_RESULT s2n_quic_write_handshake_message(struct s2n_connection *conn);
