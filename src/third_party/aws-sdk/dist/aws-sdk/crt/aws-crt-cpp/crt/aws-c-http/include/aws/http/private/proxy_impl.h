#ifndef AWS_HTTP_PROXY_IMPL_H
#define AWS_HTTP_PROXY_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

#include <aws/common/hash_table.h>
#include <aws/http/connection.h>
#include <aws/http/proxy.h>
#include <aws/http/status_code.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/socket.h>

struct aws_http_connection_manager_options;
struct aws_http_message;
struct aws_channel_slot;
struct aws_string;
struct aws_tls_connection_options;
struct aws_http_proxy_negotiator;
struct aws_http_proxy_strategy;
struct aws_http_proxy_strategy_tunneling_sequence_options;
struct aws_http_proxy_strategy_tunneling_kerberos_options;
struct aws_http_proxy_strategy_tunneling_ntlm_options;

/*
 * (Successful) State transitions for proxy connections
 *
 * Http : None -> Socket Connect -> Success
 * Https: None -> Socket Connect -> Http Connect -> Tls Negotiation -> Success
 */
enum aws_proxy_bootstrap_state {
    AWS_PBS_NONE = 0,
    AWS_PBS_SOCKET_CONNECT,
    AWS_PBS_HTTP_CONNECT,
    AWS_PBS_TLS_NEGOTIATION,
    AWS_PBS_SUCCESS,
    AWS_PBS_FAILURE,
};

/**
 * A persistent copy of the aws_http_proxy_options struct.  Clones everything appropriate.
 */
struct aws_http_proxy_config {

    struct aws_allocator *allocator;

    enum aws_http_proxy_connection_type connection_type;

    struct aws_byte_buf host;

    uint32_t port;

    struct aws_tls_connection_options *tls_options;

    struct aws_http_proxy_strategy *proxy_strategy;
};

/*
 * When a proxy connection is made, we wrap the user-supplied user data with this
 * proxy user data.  Callbacks are passed properly to the user.  By having this data
 * available, the proxy request transform that was attached to the connection can extract
 * the proxy settings it needs in order to properly transform the requests.
 *
 * Another possibility would be to fold this data into the connection itself.
 */
struct aws_http_proxy_user_data {
    struct aws_allocator *allocator;

    /*
     * dynamic proxy connection resolution state
     */
    enum aws_proxy_bootstrap_state state;
    int error_code;
    enum aws_http_status_code connect_status_code;

    /*
     * The initial http connection object between the client and the proxy.
     */
    struct aws_http_connection *proxy_connection;

    /*
     * The http connection object that gets surfaced to callers if http is the final protocol of proxy
     * negotiation.
     *
     * In the case of a forwarding proxy, proxy_connection and final_connection are the same.
     */
    struct aws_http_connection *final_connection;
    struct aws_http_message *connect_request;
    struct aws_http_stream *connect_stream;
    struct aws_http_proxy_negotiator *proxy_negotiator;

    /*
     * Cached original connect options
     */
    struct aws_string *original_host;
    uint32_t original_port;
    void *original_user_data;
    struct aws_tls_connection_options *original_tls_options;
    struct aws_client_bootstrap *original_bootstrap;
    struct aws_socket_options original_socket_options;
    bool original_manual_window_management;
    size_t original_initial_window_size;
    bool prior_knowledge_http2;
    struct aws_http1_connection_options original_http1_options;
    struct aws_http2_connection_options
        original_http2_options; /* the resource within options are allocated with userdata */
    struct aws_hash_table alpn_string_map;
    /*
     * setup/shutdown callbacks.  We enforce via fatal assert that either the http callbacks are supplied or
     * the channel callbacks are supplied but never both.
     *
     * When using a proxy to ultimately establish an http connection, use the http callbacks.
     * When using a proxy to establish any other protocol connection, use the raw channel callbacks.
     *
     * In the future, we might consider a further refactor which only use raw channel callbacks.
     */
    aws_http_on_client_connection_setup_fn *original_http_on_setup;
    aws_http_on_client_connection_shutdown_fn *original_http_on_shutdown;
    aws_client_bootstrap_on_channel_event_fn *original_channel_on_setup;
    aws_client_bootstrap_on_channel_event_fn *original_channel_on_shutdown;

    struct aws_http_proxy_config *proxy_config;

    struct aws_event_loop *requested_event_loop;

    const struct aws_host_resolution_config *host_resolution_config;
};

/* vtable of functions that proxy uses to interact with external systems.
 * tests override the vtable to mock those systems */
struct aws_http_proxy_system_vtable {
    int (*aws_channel_setup_client_tls)(
        struct aws_channel_slot *right_of_slot,
        struct aws_tls_connection_options *tls_options);
};

AWS_EXTERN_C_BEGIN

AWS_HTTP_API
struct aws_http_proxy_user_data *aws_http_proxy_user_data_new(
    struct aws_allocator *allocator,
    const struct aws_http_client_connection_options *options,
    aws_client_bootstrap_on_channel_event_fn *on_channel_setup,
    aws_client_bootstrap_on_channel_event_fn *on_channel_shutdown);

AWS_HTTP_API
void aws_http_proxy_user_data_destroy(struct aws_http_proxy_user_data *user_data);

AWS_HTTP_API
int aws_http_client_connect_via_proxy(const struct aws_http_client_connection_options *options);

AWS_HTTP_API
int aws_http_rewrite_uri_for_proxy_request(
    struct aws_http_message *request,
    struct aws_http_proxy_user_data *proxy_user_data);

AWS_HTTP_API
void aws_http_proxy_system_set_vtable(struct aws_http_proxy_system_vtable *vtable);

/**
 * Checks if tunneling proxy negotiation should continue to try and connect
 * @param proxy_negotiator negotiator to query
 * @return true if another connect request should be attempted, false otherwise
 */
AWS_HTTP_API
enum aws_http_proxy_negotiation_retry_directive aws_http_proxy_negotiator_get_retry_directive(
    struct aws_http_proxy_negotiator *proxy_negotiator);

/**
 * Constructor for a tunnel-only proxy strategy that applies no changes to outbound CONNECT requests.  Intended to be
 * the first link in an adaptive sequence for a tunneling proxy: first try a basic CONNECT, then based on the response,
 * later links are allowed to make attempts.
 *
 * @param allocator memory allocator to use
 * @return a new proxy strategy if successfully constructed, otherwise NULL
 */
AWS_HTTP_API
struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_one_time_identity(
    struct aws_allocator *allocator);

/**
 * Constructor for a forwarding-only proxy strategy that does nothing. Exists so that all proxy logic uses a
 * strategy.
 *
 * @param allocator memory allocator to use
 * @return a new proxy strategy if successfully constructed, otherwise NULL
 */
AWS_HTTP_API
struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_forwarding_identity(struct aws_allocator *allocator);

/**
 * Constructor for a tunneling proxy strategy that contains a set of sub-strategies which are tried
 * sequentially in order.  Each strategy has the choice to either proceed on a fresh connection or
 * reuse the current one.
 *
 * @param allocator memory allocator to use
 * @param config sequence configuration options
 * @return a new proxy strategy if successfully constructed, otherwise NULL
 */
AWS_HTTP_API
struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_sequence(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_sequence_options *config);

/**
 * A constructor for a proxy strategy that performs kerberos authentication by adding the appropriate
 * header and header value to CONNECT requests.
 *
 * Currently only supports synchronous fetch of kerberos token values.
 *
 * @param allocator memory allocator to use
 * @param config kerberos authentication configuration info
 * @return a new proxy strategy if successfully constructed, otherwise NULL
 */
AWS_HTTP_API
struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_kerberos(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_kerberos_options *config);

/**
 * Constructor for an NTLM proxy strategy.  Because ntlm is a challenge-response authentication protocol, this
 * strategy will only succeed in a chain in a non-leading position.  The strategy extracts the challenge from the
 * proxy's response to a previous CONNECT request in the chain.
 *
 * Currently only supports synchronous fetch of token values.
 *
 * @param allocator memory allocator to use
 * @param config configuration options for the strategy
 * @return a new proxy strategy if successfully constructed, otherwise NULL
 */
AWS_HTTP_API
struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_ntlm(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_ntlm_options *config);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_PROXY_IMPL_H */
