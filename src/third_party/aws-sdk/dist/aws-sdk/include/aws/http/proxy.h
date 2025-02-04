#ifndef AWS_PROXY_H
#define AWS_PROXY_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/ref_count.h>
#include <aws/http/http.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_http_client_connection_options;
struct aws_http_connection_manager_options;

struct aws_http_message;
struct aws_http_header;

struct aws_http_proxy_config;
struct aws_http_proxy_negotiator;
struct aws_http_proxy_strategy;

struct aws_socket_channel_bootstrap_options;

/**
 * @Deprecated - Supported proxy authentication modes.  Superceded by proxy strategy.
 */
enum aws_http_proxy_authentication_type {
    AWS_HPAT_NONE = 0,
    AWS_HPAT_BASIC,
};

enum aws_http_proxy_env_var_type {
    /**
     * Default.
     * Disable reading from environment variable for proxy.
     */
    AWS_HPEV_DISABLE = 0,
    /**
     * Enable get proxy URL from environment variable, when the manual proxy options of connection manager is not set.
     * env HTTPS_PROXY/https_proxy will be checked when the main connection use tls.
     * env HTTP_PROXY/http_proxy will be checked when the main connection NOT use tls.
     * The lower case version has precedence.
     */
    AWS_HPEV_ENABLE,
};

/**
 * Supported proxy connection types
 */
enum aws_http_proxy_connection_type {
    /**
     * Deprecated, but 0-valued for backwards compatibility
     *
     * If tls options are provided (for the main connection) then treat the proxy as a tunneling proxy
     * If tls options are not provided (for the main connection), then treat the proxy as a forwarding proxy
     */
    AWS_HPCT_HTTP_LEGACY = 0,

    /**
     * Use the proxy to forward http requests.  Attempting to use both this mode and TLS on the tunnel destination
     * is a configuration error.
     */
    AWS_HPCT_HTTP_FORWARD,

    /**
     * Use the proxy to establish a connection to a remote endpoint via a CONNECT request through the proxy.
     * Works for both plaintext and tls connections.
     */
    AWS_HPCT_HTTP_TUNNEL,
};

/*
 * Configuration for using proxy from environment variable.
 * Zero out as default settings.
 */
struct proxy_env_var_settings {
    enum aws_http_proxy_env_var_type env_var_type;
    /*
     * Optional.
     * If not set:
     * If tls options are provided (for the main connection) use tunnel proxy type
     * If tls options are not provided (for the main connection) use forward proxy type
     */
    enum aws_http_proxy_connection_type connection_type;
    /*
     * Optional.
     * If not set, a default tls option will be created. when https used for Local to proxy connection.
     * Must be distinct from the the tls_connection_options from aws_http_connection_manager_options
     */
    const struct aws_tls_connection_options *tls_options;
};

struct aws_http_proxy_strategy;

/**
 * Options for http proxy server usage
 */
struct aws_http_proxy_options {

    /**
     * Type of proxy connection to make
     */
    enum aws_http_proxy_connection_type connection_type;

    /**
     * Proxy host to connect to
     */
    struct aws_byte_cursor host;

    /**
     * Port to make the proxy connection to
     */
    uint32_t port;

    /**
     * Optional.
     * TLS configuration for the Local <-> Proxy connection
     * Must be distinct from the the TLS options in the parent aws_http_connection_options struct
     */
    const struct aws_tls_connection_options *tls_options;

    /**
     * Optional
     * Advanced option that allows the user to create a custom strategy that gives low-level control of
     * certain logical flows within the proxy logic.
     *
     * For tunneling proxies it allows custom retry and adaptive negotiation of CONNECT requests.
     * For forwarding proxies it allows custom request transformations.
     */
    struct aws_http_proxy_strategy *proxy_strategy;

    /**
     * @Deprecated - What type of proxy authentication to use, if any.
     * Replaced by instantiating a proxy_strategy
     */
    enum aws_http_proxy_authentication_type auth_type;

    /**
     * @Deprecated - Optional user name to use for basic authentication
     * Replaced by instantiating a proxy_strategy via aws_http_proxy_strategy_new_basic_auth()
     */
    struct aws_byte_cursor auth_username;

    /**
     * @Deprecated - Optional password to use for basic authentication
     * Replaced by instantiating a proxy_strategy via aws_http_proxy_strategy_new_basic_auth()
     */
    struct aws_byte_cursor auth_password;
};

/**
 * Synchronous (for now) callback function to fetch a token used in modifying CONNECT requests
 */
typedef struct aws_string *(aws_http_proxy_negotiation_get_token_sync_fn)(void *user_data, int *out_error_code);

/**
 * Synchronous (for now) callback function to fetch a token used in modifying CONNECT request.  Includes a (byte string)
 * context intended to be used as part of a challenge-response flow.
 */
typedef struct aws_string *(
    aws_http_proxy_negotiation_get_challenge_token_sync_fn)(void *user_data,
                                                            const struct aws_byte_cursor *challenge_context,
                                                            int *out_error_code);

/**
 * Proxy negotiation logic must call this function to indicate an unsuccessful outcome
 */
typedef void(aws_http_proxy_negotiation_terminate_fn)(
    struct aws_http_message *message,
    int error_code,
    void *internal_proxy_user_data);

/**
 * Proxy negotiation logic must call this function to forward the potentially-mutated request back to the proxy
 * connection logic.
 */
typedef void(aws_http_proxy_negotiation_http_request_forward_fn)(
    struct aws_http_message *message,
    void *internal_proxy_user_data);

/**
 * User-supplied transform callback which implements the proxy request flow and ultimately, across all execution
 * pathways, invokes either the terminate function or the forward function appropriately.
 *
 * For tunneling proxy connections, this request flow transform only applies to the CONNECT stage of proxy
 * connection establishment.
 *
 * For forwarding proxy connections, this request flow transform applies to every single http request that goes
 * out on the connection.
 *
 * Forwarding proxy connections cannot yet support a truly async request transform without major surgery on http
 * stream creation, so for now, we split into an async version (for tunneling proxies) and a separate
 * synchronous version for forwarding proxies.  Also forwarding proxies are a kind of legacy dead-end in some
 * sense.
 *
 */
typedef void(aws_http_proxy_negotiation_http_request_transform_async_fn)(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message,
    aws_http_proxy_negotiation_terminate_fn *negotiation_termination_callback,
    aws_http_proxy_negotiation_http_request_forward_fn *negotiation_http_request_forward_callback,
    void *internal_proxy_user_data);

typedef int(aws_http_proxy_negotiation_http_request_transform_fn)(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message);

/**
 * Tunneling proxy connections only.  A callback that lets the negotiator examine the headers in the
 * response to the most recent CONNECT request as they arrive.
 */
typedef int(aws_http_proxy_negotiation_connect_on_incoming_headers_fn)(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers);

/**
 * Tunneling proxy connections only.  A callback that lets the negotiator examine the status code of the
 * response to the most recent CONNECT request.
 */
typedef int(aws_http_proxy_negotiator_connect_status_fn)(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_status_code status_code);

/**
 * Tunneling proxy connections only.  A callback that lets the negotiator examine the body of the response
 * to the most recent CONNECT request.
 */
typedef int(aws_http_proxy_negotiator_connect_on_incoming_body_fn)(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    const struct aws_byte_cursor *data);

/*
 * Control value that lets the http proxy implementation know if and how to retry a CONNECT request based on
 * the proxy negotiator's state.
 */
enum aws_http_proxy_negotiation_retry_directive {
    /*
     * Stop trying to connect through the proxy and give up.
     */
    AWS_HPNRD_STOP,

    /*
     * Establish a new connection to the proxy before making the next CONNECT request
     */
    AWS_HPNRD_NEW_CONNECTION,

    /*
     * Reuse the existing connection to make the next CONNECT request
     */
    AWS_HPNRD_CURRENT_CONNECTION,
};

typedef enum aws_http_proxy_negotiation_retry_directive(aws_http_proxy_negotiator_get_retry_directive_fn)(
    struct aws_http_proxy_negotiator *proxy_negotiator);

/**
 * Vtable for forwarding-based proxy negotiators
 */
struct aws_http_proxy_negotiator_forwarding_vtable {
    aws_http_proxy_negotiation_http_request_transform_fn *forward_request_transform;
};

/**
 * Vtable for tunneling-based proxy negotiators
 */
struct aws_http_proxy_negotiator_tunnelling_vtable {
    aws_http_proxy_negotiation_http_request_transform_async_fn *connect_request_transform;

    aws_http_proxy_negotiation_connect_on_incoming_headers_fn *on_incoming_headers_callback;
    aws_http_proxy_negotiator_connect_status_fn *on_status_callback;
    aws_http_proxy_negotiator_connect_on_incoming_body_fn *on_incoming_body_callback;

    aws_http_proxy_negotiator_get_retry_directive_fn *get_retry_directive;
};

/*
 * Base definition of a proxy negotiator.
 *
 * A negotiator works differently based on what kind of proxy connection is being asked for:
 *
 * (1) Tunneling - In a tunneling proxy connection, the connect_request_transform is invoked on every CONNECT request.
 * The connect_request_transform implementation *MUST*, in turn, eventually call one of the terminate or forward
 * functions it gets supplied with.
 *
 *  Every CONNECT request, if a response is obtained, will properly invoke the response handling callbacks supplied
 *  in the tunneling vtable.
 *
 * (2) Forwarding - In a forwarding proxy connection, the forward_request_transform is invoked on every request sent out
 * on the connection.
 */
struct aws_http_proxy_negotiator {
    struct aws_ref_count ref_count;

    void *impl;

    union {
        struct aws_http_proxy_negotiator_forwarding_vtable *forwarding_vtable;
        struct aws_http_proxy_negotiator_tunnelling_vtable *tunnelling_vtable;
    } strategy_vtable;
};

/*********************************************************************************************/

typedef struct aws_http_proxy_negotiator *(
    aws_http_proxy_strategy_create_negotiator_fn)(struct aws_http_proxy_strategy *proxy_strategy,
                                                  struct aws_allocator *allocator);

struct aws_http_proxy_strategy_vtable {
    aws_http_proxy_strategy_create_negotiator_fn *create_negotiator;
};

struct aws_http_proxy_strategy {
    struct aws_ref_count ref_count;
    struct aws_http_proxy_strategy_vtable *vtable;
    void *impl;
    enum aws_http_proxy_connection_type proxy_connection_type;
};

/*
 * Options necessary to create a basic authentication proxy strategy
 */
struct aws_http_proxy_strategy_basic_auth_options {

    /* type of proxy connection being established, must be forwarding or tunnel */
    enum aws_http_proxy_connection_type proxy_connection_type;

    /* user name to use in basic authentication */
    struct aws_byte_cursor user_name;

    /* password to use in basic authentication */
    struct aws_byte_cursor password;
};

/*
 * Options necessary to create a (synchronous) kerberos authentication proxy strategy
 */
struct aws_http_proxy_strategy_tunneling_kerberos_options {

    aws_http_proxy_negotiation_get_token_sync_fn *get_token;

    void *get_token_user_data;
};

/*
 * Options necessary to create a (synchronous) ntlm authentication proxy strategy
 */
struct aws_http_proxy_strategy_tunneling_ntlm_options {

    aws_http_proxy_negotiation_get_token_sync_fn *get_token;

    aws_http_proxy_negotiation_get_challenge_token_sync_fn *get_challenge_token;

    void *get_challenge_token_user_data;
};

/*
 * Options necessary to create an adaptive sequential strategy that tries one or more of kerberos and ntlm (in that
 * order, if both are active).  If an options struct is NULL, then that strategy will not be used.
 */
struct aws_http_proxy_strategy_tunneling_adaptive_options {
    /*
     * If non-null, will insert a kerberos proxy strategy into the adaptive sequence
     */
    struct aws_http_proxy_strategy_tunneling_kerberos_options *kerberos_options;

    /*
     * If non-null will insert an ntlm proxy strategy into the adaptive sequence
     */
    struct aws_http_proxy_strategy_tunneling_ntlm_options *ntlm_options;
};

/*
 * Options necessary to create a sequential proxy strategy.
 */
struct aws_http_proxy_strategy_tunneling_sequence_options {
    struct aws_http_proxy_strategy **strategies;

    uint32_t strategy_count;
};

AWS_EXTERN_C_BEGIN

/**
 * Take a reference to an http proxy negotiator
 * @param proxy_negotiator negotiator to take a reference to
 * @return the strategy
 */
AWS_HTTP_API
struct aws_http_proxy_negotiator *aws_http_proxy_negotiator_acquire(struct aws_http_proxy_negotiator *proxy_negotiator);

/**
 * Release a reference to an http proxy negotiator
 * @param proxy_negotiator negotiator to release a reference to
 */
AWS_HTTP_API
void aws_http_proxy_negotiator_release(struct aws_http_proxy_negotiator *proxy_negotiator);

/**
 * Creates a new proxy negotiator from a proxy strategy
 * @param allocator memory allocator to use
 * @param strategy strategy to creation a new negotiator for
 * @return a new proxy negotiator if successful, otherwise NULL
 */
AWS_HTTP_API
struct aws_http_proxy_negotiator *aws_http_proxy_strategy_create_negotiator(
    struct aws_http_proxy_strategy *strategy,
    struct aws_allocator *allocator);

/**
 * Take a reference to an http proxy strategy
 * @param proxy_strategy strategy to take a reference to
 * @return the strategy
 */
AWS_HTTP_API
struct aws_http_proxy_strategy *aws_http_proxy_strategy_acquire(struct aws_http_proxy_strategy *proxy_strategy);

/**
 * Release a reference to an http proxy strategy
 * @param proxy_strategy strategy to release a reference to
 */
AWS_HTTP_API
void aws_http_proxy_strategy_release(struct aws_http_proxy_strategy *proxy_strategy);

/**
 * A constructor for a proxy strategy that performs basic authentication by adding the appropriate
 * header and header value to requests or CONNECT requests.
 *
 * @param allocator memory allocator to use
 * @param config basic authentication configuration info
 * @return a new proxy strategy if successfully constructed, otherwise NULL
 */
AWS_HTTP_API
struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_basic_auth(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_basic_auth_options *config);

/**
 * Constructor for an adaptive tunneling proxy strategy.  This strategy attempts a vanilla CONNECT and if that
 * fails it may make followup CONNECT attempts using kerberos or ntlm tokens, based on configuration and proxy
 * response properties.
 *
 * @param allocator memory allocator to use
 * @param config configuration options for the strategy
 * @return a new proxy strategy if successfully constructed, otherwise NULL
 */
AWS_HTTP_API
struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_adaptive(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_adaptive_options *config);

/*
 * aws_http_proxy_config is the persistent, memory-managed version of aws_http_proxy_options
 *
 * This is a set of APIs for creating, destroying and converting between them
 */

/**
 * Create a persistent proxy configuration from http connection options
 * @param allocator memory allocator to use
 * @param options http connection options to source proxy configuration from
 * @return
 */
AWS_HTTP_API
struct aws_http_proxy_config *aws_http_proxy_config_new_from_connection_options(
    struct aws_allocator *allocator,
    const struct aws_http_client_connection_options *options);

/**
 * Create a persistent proxy configuration from http connection manager options
 * @param allocator memory allocator to use
 * @param options http connection manager options to source proxy configuration from
 * @return
 */
AWS_HTTP_API
struct aws_http_proxy_config *aws_http_proxy_config_new_from_manager_options(
    struct aws_allocator *allocator,
    const struct aws_http_connection_manager_options *options);

/**
 * Create a persistent proxy configuration from non-persistent proxy options.  The resulting
 * proxy configuration assumes a tunneling connection type.
 *
 * @param allocator memory allocator to use
 * @param options http proxy options to source proxy configuration from
 * @return
 */
AWS_HTTP_API
struct aws_http_proxy_config *aws_http_proxy_config_new_tunneling_from_proxy_options(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_options *options);

/**
 * Create a persistent proxy configuration from non-persistent proxy options.
 * Legacy connection type of proxy options will be rejected.
 *
 * @param allocator memory allocator to use
 * @param options http proxy options to source proxy configuration from
 * @return
 */
AWS_HTTP_API
struct aws_http_proxy_config *aws_http_proxy_config_new_from_proxy_options(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_options *options);

/**
 * Create a persistent proxy configuration from non-persistent proxy options.
 *
 * @param allocator memory allocator to use
 * @param options http proxy options to source proxy configuration from
 * @param is_tls_connection tls connection info of the main connection to determine connection_type
 *                          when the connection_type is legacy.
 * @return
 */
AWS_HTTP_API
struct aws_http_proxy_config *aws_http_proxy_config_new_from_proxy_options_with_tls_info(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_options *proxy_options,
    bool is_tls_connection);

/**
 * Clones an existing proxy configuration.  A refactor could remove this (do a "move" between the old and new user
 * data in the one spot it's used) but that should wait until we have better test cases for the logic where this
 * gets invoked (ntlm/kerberos chains).
 *
 * @param allocator memory allocator to use
 * @param proxy_config http proxy configuration to clone
 * @return
 */
AWS_HTTP_API
struct aws_http_proxy_config *aws_http_proxy_config_new_clone(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_config *proxy_config);

/**
 * Destroys an http proxy configuration
 * @param config http proxy configuration to destroy
 */
AWS_HTTP_API
void aws_http_proxy_config_destroy(struct aws_http_proxy_config *config);

/**
 * Initializes non-persistent http proxy options from a persistent http proxy configuration
 * @param options http proxy options to initialize
 * @param config the http proxy config to use as an initialization source
 */
AWS_HTTP_API
void aws_http_proxy_options_init_from_config(
    struct aws_http_proxy_options *options,
    const struct aws_http_proxy_config *config);

/**
 * Establish an arbitrary protocol connection through an http proxy via tunneling CONNECT.  Alpn is
 * not required for this connection process to succeed, but we encourage its use if available.
 *
 * @param channel_options configuration options for the socket level connection
 * @param proxy_options configuration options for the proxy connection
 *
 * @return AWS_OP_SUCCESS if the asynchronous channel kickoff succeeded, AWS_OP_ERR otherwise
 */
AWS_HTTP_API int aws_http_proxy_new_socket_channel(
    struct aws_socket_channel_bootstrap_options *channel_options,
    const struct aws_http_proxy_options *proxy_options);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_PROXY_STRATEGY_H */
