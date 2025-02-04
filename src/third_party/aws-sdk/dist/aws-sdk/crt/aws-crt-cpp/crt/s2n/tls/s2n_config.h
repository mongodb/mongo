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

#include <sys/param.h>

#include "api/s2n.h"
#include "crypto/s2n_certificate.h"
#include "crypto/s2n_dhe.h"
#include "tls/s2n_crl.h"
#include "tls/s2n_key_update.h"
#include "tls/s2n_psk.h"
#include "tls/s2n_record.h"
#include "tls/s2n_renegotiate.h"
#include "tls/s2n_resume.h"
#include "tls/s2n_tls_parameters.h"
#include "tls/s2n_x509_validator.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_set.h"

#define S2N_MAX_TICKET_KEYS       48
#define S2N_MAX_TICKET_KEY_HASHES 500 /* 10KB */

/*
 * TLS1.3 does not allow alert messages to be fragmented, and some TLS
 * implementations (for example, GnuTLS) reject fragmented TLS1.2 alerts.
 * The send buffer must be able to hold an unfragmented alert message.
 *
 * We choose not to fragment KeyUpdate messages to keep our post-handshake
 * fragmentation logic simple and consistent across message types.
 * The send buffer must be able to hold an unfragmented KeyUpdate message.
 */
#define S2N_MIN_SEND_BUFFER_FRAGMENT_SIZE MAX(S2N_KEY_UPDATE_MESSAGE_SIZE, S2N_ALERT_LENGTH)
#define S2N_MIN_SEND_BUFFER_SIZE          S2N_TLS_MAX_RECORD_LEN_FOR(S2N_MIN_SEND_BUFFER_FRAGMENT_SIZE)

struct s2n_cipher_preferences;

typedef enum {
    S2N_NOT_OWNED = 0,
    S2N_APP_OWNED,
    S2N_LIB_OWNED,
} s2n_cert_ownership;

struct s2n_config {
    unsigned use_tickets : 1;

    /* Whether a connection can be used by a QUIC implementation.
     * See s2n_quic_support.h */
    unsigned quic_enabled : 1;

    unsigned default_certs_are_explicit : 1;
    unsigned use_session_cache : 1;
    /* if this is FALSE, server will ignore client's Maximum Fragment Length request */
    unsigned accept_mfl : 1;
    unsigned check_ocsp : 1;
    unsigned disable_x509_time_validation : 1;
    unsigned disable_x509_validation : 1;
    unsigned max_verify_cert_chain_depth_set : 1;
    /* Whether to add dss cert type during a server certificate request.
     * See s2n_config_enable_cert_req_dss_legacy_compat. */
    unsigned cert_req_dss_legacy_compat_enabled : 1;
    /* Whether any RSA certificates have been configured server-side to send to clients. This is needed so that the
     * server knows whether or not to self-downgrade to TLS 1.2 if the server is compiled with Openssl 1.0.2 and does
     * not support RSA PSS signing (which is required for TLS 1.3). */
    unsigned is_rsa_cert_configured : 1;
    /* It's possible to use a certificate without loading the private key,
     * but async signing must be enabled. Use this flag to enforce that restriction.
     */
    unsigned no_signing_key : 1;
    /*
     * Whether to verify signatures locally before sending them over the wire.
     * See s2n_config_set_verify_after_sign.
     */
    unsigned verify_after_sign : 1;

    /* Indicates support for the npn extension */
    unsigned npn_supported : 1;

    /* Indicates s2n_recv should read as much as it can into the output buffer
     *
     * Note: This defaults to false to ensure backwards compatibility with
     * applications which relied on s2n_recv returning a single record.
     */
    unsigned recv_multi_record : 1;

    /* Indicates whether the user has enabled OCSP status requests */
    unsigned ocsp_status_requested_by_user : 1;

    /* Indicates whether s2n has enabled OCSP status requests, for backwards compatibility */
    unsigned ocsp_status_requested_by_s2n : 1;

    /* TLS1.3 can be dangerous with kTLS. Require it to be explicitly enabled. */
    unsigned ktls_tls13_enabled : 1;

    unsigned custom_blinding_set : 1;

    unsigned ticket_forward_secrecy : 1;

    struct s2n_dh_params *dhparams;
    /* Needed until we can deprecate s2n_config_add_cert_chain_and_key. This is
     * used to release memory allocated only in the deprecated API that the application 
     * does not have a reference to. */
    struct s2n_map *domain_name_to_cert_map;
    struct certs_by_type default_certs_by_type;
    struct s2n_blob application_protocols;
    s2n_clock_time_nanoseconds wall_clock;
    s2n_clock_time_nanoseconds monotonic_clock;

    const struct s2n_security_policy *security_policy;

    void *sys_clock_ctx;
    void *monotonic_clock_ctx;

    s2n_client_hello_fn *client_hello_cb;
    s2n_client_hello_cb_mode client_hello_cb_mode;

    uint32_t max_blinding;

    void *client_hello_cb_ctx;

    uint64_t session_state_lifetime_in_nanos;

    struct s2n_set *ticket_keys;
    struct s2n_set *ticket_key_hashes;
    uint64_t encrypt_decrypt_key_lifetime_in_nanos;
    uint64_t decrypt_key_lifetime_in_nanos;

    /* If session cache is being used, these must all be set */
    s2n_cache_store_callback cache_store;
    void *cache_store_data;

    s2n_cache_retrieve_callback cache_retrieve;
    void *cache_retrieve_data;

    s2n_cache_delete_callback cache_delete;
    void *cache_delete_data;

    s2n_ct_support_level ct_type;

    /* Track whether the application has overriden the default client auth type.
     * Clients and servers have different default client auth behavior, and this
     * config could apply to either.
     * This should be a bitflag, but that change is blocked on the SAW proofs.
     */
    uint8_t client_cert_auth_type_overridden;

    /* Whether or not the client should authenticate itself to the server.
     * Only used if client_cert_auth_type_overridden is true.
     */
    s2n_cert_auth_type client_cert_auth_type;

    s2n_alert_behavior alert_behavior;

    /* Return TRUE if the host should be trusted, If FALSE this will likely be called again for every host/alternative name
     * in the certificate. If any respond TRUE. If none return TRUE, the cert will be considered untrusted. */
    s2n_verify_host_fn verify_host_fn;
    void *data_for_verify_host;

    s2n_crl_lookup_callback crl_lookup_cb;
    void *crl_lookup_ctx;

    s2n_cert_validation_callback cert_validation_cb;
    void *cert_validation_ctx;

    /* Application supplied callback to resolve domain name conflicts when loading certs. */
    s2n_cert_tiebreak_callback cert_tiebreak_cb;

    uint8_t mfl_code;

    uint8_t initial_tickets_to_send;

    struct s2n_x509_trust_store trust_store;
    uint16_t max_verify_cert_chain_depth;

    s2n_async_pkey_fn async_pkey_cb;

    s2n_psk_selection_callback psk_selection_cb;
    void *psk_selection_ctx;

    s2n_key_log_fn key_log_cb;
    void *key_log_ctx;

    s2n_session_ticket_fn session_ticket_cb;
    void *session_ticket_ctx;

    s2n_early_data_cb early_data_cb;

    uint32_t server_max_early_data_size;

    s2n_psk_mode psk_mode;

    s2n_async_pkey_validation_mode async_pkey_validation_mode;

    /* The user defined context associated with config */
    void *context;

    s2n_cert_ownership cert_ownership;

    /* Used to override the stuffer size for a connection's `out` stuffer. */
    uint32_t send_buffer_size_override;

    void *renegotiate_request_ctx;
    s2n_renegotiate_request_cb renegotiate_request_cb;

    /* This version is meant as a safeguard against future TLS features which might affect the connection
     * serialization feature.
     *
     * For example, suppose that a new TLS parameter is released which affects how data is sent
     * post-handshake. This parameter must be available in both the s2n-tls version that serializes the 
     * connection, as well as the version that deserializes the connection. If not, the serializer
     * may negotiate this feature with its peer, which would cause an older deserializer to run into errors
     * sending data to the peer.
     * 
     * This kind of version-mismatch can happen during deployments and rollbacks, and therefore we require
     * the user to tell us which serialized version they support pre-handshake. 
     * We will not negotiate a new feature until the user requests the serialized connection
     * version the feature is tied to (i.e. the request indicates they have finished deploying
     * the new feature to their entire fleet.)
     */
    s2n_serialization_version serialized_connection_version;

    /* List of certificate authorities supported */
    struct s2n_blob cert_authorities;
};

S2N_CLEANUP_RESULT s2n_config_ptr_free(struct s2n_config **config);

int s2n_config_defaults_init(void);
S2N_RESULT s2n_config_testing_defaults_init_tls13_certs(void);
struct s2n_config *s2n_fetch_default_config(void);
int s2n_config_set_unsafe_for_testing(struct s2n_config *config);

int s2n_config_init_session_ticket_keys(struct s2n_config *config);
int s2n_config_free_session_ticket_keys(struct s2n_config *config);

void s2n_wipe_static_configs(void);
struct s2n_cert_chain_and_key *s2n_config_get_single_default_cert(struct s2n_config *config);
int s2n_config_get_num_default_certs(const struct s2n_config *config);
S2N_RESULT s2n_config_wall_clock(struct s2n_config *config, uint64_t *output);

/* Validate that the certificates in `config` respect the certificate preferences
 * in `security_policy` */
S2N_RESULT s2n_config_validate_loaded_certificates(const struct s2n_config *config,
        const struct s2n_security_policy *security_policy);
