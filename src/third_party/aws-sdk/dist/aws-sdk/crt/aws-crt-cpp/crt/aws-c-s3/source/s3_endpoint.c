/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_util.h"

#include <aws/auth/credentials.h>
#include <aws/common/assert.h>
#include <aws/common/device_random.h>
#include <aws/common/string.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#include <inttypes.h>

static const uint32_t s_connection_timeout_ms = 3000;
static const uint32_t s_http_port = 80;
static const uint32_t s_https_port = 443;

static void s_s3_endpoint_on_host_resolver_address_resolved(
    struct aws_host_resolver *resolver,
    const struct aws_string *host_name,
    int err_code,
    const struct aws_array_list *host_addresses,
    void *user_data);

static struct aws_http_connection_manager *s_s3_endpoint_create_http_connection_manager(
    struct aws_s3_endpoint *endpoint,
    const struct aws_string *host_name,
    struct aws_client_bootstrap *client_bootstrap,
    const struct aws_tls_connection_options *tls_connection_options,
    uint32_t max_connections,
    uint32_t port,
    const struct aws_http_proxy_config *proxy_config,
    const struct proxy_env_var_settings *proxy_ev_settings,
    uint32_t connect_timeout_ms,
    const struct aws_s3_tcp_keep_alive_options *tcp_keep_alive_options,
    const struct aws_http_connection_monitoring_options *monitoring_options,
    const struct aws_byte_cursor *network_interface_names_array,
    size_t num_network_interface_names);

static void s_s3_endpoint_http_connection_manager_shutdown_callback(void *user_data);

static void s_s3_endpoint_acquire(struct aws_s3_endpoint *endpoint, bool already_holding_lock);

static void s_s3_endpoint_release(struct aws_s3_endpoint *endpoint);

static const struct aws_s3_endpoint_system_vtable s_s3_endpoint_default_system_vtable = {
    .acquire = s_s3_endpoint_acquire,
    .release = s_s3_endpoint_release,
};

static const struct aws_s3_endpoint_system_vtable *s_s3_endpoint_system_vtable = &s_s3_endpoint_default_system_vtable;

void aws_s3_endpoint_set_system_vtable(const struct aws_s3_endpoint_system_vtable *vtable) {
    s_s3_endpoint_system_vtable = vtable;
}

struct aws_s3_endpoint *aws_s3_endpoint_new(
    struct aws_allocator *allocator,
    const struct aws_s3_endpoint_options *options) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->host_name);

    struct aws_s3_endpoint *endpoint = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_endpoint));
    endpoint->client_synced_data.ref_count = 1;

    endpoint->allocator = allocator;
    endpoint->host_name = options->host_name;

    struct aws_host_resolution_config host_resolver_config;
    AWS_ZERO_STRUCT(host_resolver_config);
    host_resolver_config.impl = aws_default_dns_resolve;
    host_resolver_config.max_ttl = options->dns_host_address_ttl_seconds;
    host_resolver_config.impl_data = NULL;

    if (aws_host_resolver_resolve_host(
            options->client_bootstrap->host_resolver,
            endpoint->host_name,
            s_s3_endpoint_on_host_resolver_address_resolved,
            &host_resolver_config,
            NULL)) {

        AWS_LOGF_ERROR(
            AWS_LS_S3_ENDPOINT,
            "id=%p: Error trying to resolve host for endpoint %s",
            (void *)endpoint,
            (const char *)endpoint->host_name->bytes);

        goto error_cleanup;
    }

    endpoint->http_connection_manager = s_s3_endpoint_create_http_connection_manager(
        endpoint,
        options->host_name,
        options->client_bootstrap,
        options->tls_connection_options,
        options->max_connections,
        options->port,
        options->proxy_config,
        options->proxy_ev_settings,
        options->connect_timeout_ms,
        options->tcp_keep_alive_options,
        options->monitoring_options,
        options->network_interface_names_array,
        options->num_network_interface_names);

    if (endpoint->http_connection_manager == NULL) {
        goto error_cleanup;
    }

    endpoint->client = options->client;

    return endpoint;

error_cleanup:

    aws_mem_release(allocator, endpoint);

    return NULL;
}

static struct aws_http_connection_manager *s_s3_endpoint_create_http_connection_manager(
    struct aws_s3_endpoint *endpoint,
    const struct aws_string *host_name,
    struct aws_client_bootstrap *client_bootstrap,
    const struct aws_tls_connection_options *tls_connection_options,
    uint32_t max_connections,
    uint32_t port,
    const struct aws_http_proxy_config *proxy_config,
    const struct proxy_env_var_settings *proxy_ev_settings,
    uint32_t connect_timeout_ms,
    const struct aws_s3_tcp_keep_alive_options *tcp_keep_alive_options,
    const struct aws_http_connection_monitoring_options *monitoring_options,
    const struct aws_byte_cursor *network_interface_names_array,
    size_t num_network_interface_names) {

    AWS_PRECONDITION(endpoint);
    AWS_PRECONDITION(client_bootstrap);
    AWS_PRECONDITION(host_name);

    struct aws_byte_cursor host_name_cursor = aws_byte_cursor_from_string(host_name);

    /* Try to set up an HTTP connection manager. */
    struct aws_socket_options socket_options;
    AWS_ZERO_STRUCT(socket_options);
    socket_options.type = AWS_SOCKET_STREAM;
    socket_options.domain = AWS_SOCKET_IPV4;
    socket_options.connect_timeout_ms = connect_timeout_ms == 0 ? s_connection_timeout_ms : connect_timeout_ms;
    if (tcp_keep_alive_options != NULL) {
        socket_options.keepalive = true;
        socket_options.keep_alive_interval_sec = tcp_keep_alive_options->keep_alive_interval_sec;
        socket_options.keep_alive_timeout_sec = tcp_keep_alive_options->keep_alive_timeout_sec;
        socket_options.keep_alive_max_failed_probes = tcp_keep_alive_options->keep_alive_max_failed_probes;
    }
    struct proxy_env_var_settings proxy_ev_settings_default;
    /* Turn on environment variable for proxy by default */
    if (proxy_ev_settings == NULL) {
        AWS_ZERO_STRUCT(proxy_ev_settings_default);
        proxy_ev_settings_default.env_var_type = AWS_HPEV_ENABLE;
        proxy_ev_settings = &proxy_ev_settings_default;
    }

    struct aws_http_connection_manager_options manager_options;
    AWS_ZERO_STRUCT(manager_options);
    manager_options.bootstrap = client_bootstrap;
    manager_options.initial_window_size = SIZE_MAX;
    manager_options.socket_options = &socket_options;
    manager_options.host = host_name_cursor;
    manager_options.max_connections = max_connections;
    manager_options.shutdown_complete_callback = s_s3_endpoint_http_connection_manager_shutdown_callback;
    manager_options.shutdown_complete_user_data = endpoint;
    manager_options.proxy_ev_settings = proxy_ev_settings;
    manager_options.network_interface_names_array = network_interface_names_array;
    manager_options.num_network_interface_names = num_network_interface_names;
    if (monitoring_options != NULL) {
        manager_options.monitoring_options = monitoring_options;
    }

    struct aws_http_proxy_options proxy_options;
    if (proxy_config != NULL) {
        aws_http_proxy_options_init_from_config(&proxy_options, proxy_config);
        manager_options.proxy_options = &proxy_options;
    }

    struct aws_tls_connection_options *manager_tls_options = NULL;

    if (tls_connection_options != NULL) {
        manager_tls_options = aws_mem_calloc(endpoint->allocator, 1, sizeof(struct aws_tls_connection_options));
        aws_tls_connection_options_copy(manager_tls_options, tls_connection_options);

        /* TODO fix this in the actual aws_tls_connection_options_set_server_name function. */
        if (manager_tls_options->server_name != NULL) {
            aws_string_destroy(manager_tls_options->server_name);
            manager_tls_options->server_name = NULL;
        }

        aws_tls_connection_options_set_server_name(manager_tls_options, endpoint->allocator, &host_name_cursor);

        manager_options.tls_connection_options = manager_tls_options;
        manager_options.port = port == 0 ? s_https_port : port;
    } else {
        manager_options.port = port == 0 ? s_http_port : port;
    }

    struct aws_http_connection_manager *http_connection_manager =
        aws_http_connection_manager_new(endpoint->allocator, &manager_options);

    if (manager_tls_options != NULL) {
        aws_tls_connection_options_clean_up(manager_tls_options);
        aws_mem_release(endpoint->allocator, manager_tls_options);
        manager_tls_options = NULL;
    }

    if (http_connection_manager == NULL) {
        AWS_LOGF_ERROR(AWS_LS_S3_ENDPOINT, "id=%p: Could not create http connection manager.", (void *)endpoint);
        return NULL;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_S3_ENDPOINT,
        "id=%p: Created connection manager %p for endpoint",
        (void *)endpoint,
        (void *)http_connection_manager);

    return http_connection_manager;
}

struct aws_s3_endpoint *aws_s3_endpoint_acquire(struct aws_s3_endpoint *endpoint, bool already_holding_lock) {
    if (endpoint) {
        s_s3_endpoint_system_vtable->acquire(endpoint, already_holding_lock);
    }
    return endpoint;
}

static void s_s3_endpoint_acquire(struct aws_s3_endpoint *endpoint, bool already_holding_lock) {
    AWS_PRECONDITION(endpoint);

    if (!already_holding_lock) {
        aws_s3_client_lock_synced_data(endpoint->client);
    }

    ++endpoint->client_synced_data.ref_count;

    if (!already_holding_lock) {
        aws_s3_client_unlock_synced_data(endpoint->client);
    }
}

void aws_s3_endpoint_release(struct aws_s3_endpoint *endpoint) {
    if (endpoint) {
        s_s3_endpoint_system_vtable->release(endpoint);
    }
}

static void s_s3_endpoint_release(struct aws_s3_endpoint *endpoint) {
    AWS_PRECONDITION(endpoint);
    AWS_PRECONDITION(endpoint->client);

    /* BEGIN CRITICAL SECTION */
    aws_s3_client_lock_synced_data(endpoint->client);

    bool should_destroy = endpoint->client_synced_data.ref_count == 1 && !endpoint->client->synced_data.active;
    if (should_destroy) {
        aws_hash_table_remove(&endpoint->client->synced_data.endpoints, endpoint->host_name, NULL, NULL);
    }
    --endpoint->client_synced_data.ref_count;

    aws_s3_client_unlock_synced_data(endpoint->client);
    /* END CRITICAL SECTION */

    if (should_destroy) {
        /* Do a sync cleanup since client is getting destroyed to avoid any cleanup delay.
         * The endpoint may have async cleanup to do (connection manager).
         * When that's all done we'll invoke a completion callback.
         * Since it's a crime to hold a lock while invoking a callback,
         * we make sure that we've released the client's lock before proceeding...
         */
        aws_s3_endpoint_destroy(endpoint);
    }
}

void aws_s3_endpoint_destroy(struct aws_s3_endpoint *endpoint) {
    AWS_PRECONDITION(endpoint);
    AWS_PRECONDITION(endpoint->http_connection_manager);

    AWS_FATAL_ASSERT(endpoint->client_synced_data.ref_count == 0);

    struct aws_http_connection_manager *http_connection_manager = endpoint->http_connection_manager;
    endpoint->http_connection_manager = NULL;

    /* Cleanup continues once the manager's shutdown callback is invoked */
    aws_http_connection_manager_release(http_connection_manager);
}

static void s_s3_endpoint_http_connection_manager_shutdown_callback(void *user_data) {
    struct aws_s3_endpoint *endpoint = user_data;
    AWS_ASSERT(endpoint);

    struct aws_s3_client *client = endpoint->client;

    aws_mem_release(endpoint->allocator, endpoint);

    client->vtable->endpoint_shutdown_callback(client);
}

static void s_s3_endpoint_on_host_resolver_address_resolved(
    struct aws_host_resolver *resolver,
    const struct aws_string *host_name,
    int err_code,
    const struct aws_array_list *host_addresses,
    void *user_data) {
    (void)resolver;
    (void)host_name;
    (void)err_code;
    (void)host_addresses;
    (void)user_data;
    /* DO NOT add any logic here, unless you also ensure the endpoint lives long enough */
}
