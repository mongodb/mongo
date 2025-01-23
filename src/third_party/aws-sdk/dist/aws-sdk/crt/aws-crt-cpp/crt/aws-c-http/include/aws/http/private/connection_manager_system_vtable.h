#ifndef AWS_HTTP_CONNECTION_MANAGER_SYSTEM_VTABLE_H
#define AWS_HTTP_CONNECTION_MANAGER_SYSTEM_VTABLE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

#include <aws/http/connection.h>

struct aws_http_connection_manager;

/* vtable of functions that aws_http_connection_manager uses to interact with external systems.
 * tests override the vtable to mock those systems */
struct aws_http_connection_manager_system_vtable {
    /*
     * Downstream http functions
     */
    int (*aws_http_client_connect)(const struct aws_http_client_connection_options *options);
    void (*aws_http_connection_close)(struct aws_http_connection *connection);
    void (*aws_http_connection_release)(struct aws_http_connection *connection);
    bool (*aws_http_connection_new_requests_allowed)(const struct aws_http_connection *connection);
    int (*aws_high_res_clock_get_ticks)(uint64_t *timestamp);
    bool (*aws_channel_thread_is_callers_thread)(struct aws_channel *channel);
    struct aws_channel *(*aws_http_connection_get_channel)(struct aws_http_connection *connection);
    enum aws_http_version (*aws_http_connection_get_version)(const struct aws_http_connection *connection);
};

AWS_HTTP_API
bool aws_http_connection_manager_system_vtable_is_valid(const struct aws_http_connection_manager_system_vtable *table);

AWS_HTTP_API
void aws_http_connection_manager_set_system_vtable(
    struct aws_http_connection_manager *manager,
    const struct aws_http_connection_manager_system_vtable *system_vtable);

AWS_HTTP_API
extern const struct aws_http_connection_manager_system_vtable *g_aws_http_connection_manager_default_system_vtable_ptr;

#endif /* AWS_HTTP_CONNECTION_MANAGER_SYSTEM_VTABLE_H */
