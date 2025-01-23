/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_auto_ranged_get.h"
#include "aws/s3/private/s3_auto_ranged_put.h"
#include "aws/s3/private/s3_buffer_pool.h"
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_copy_object.h"
#include "aws/s3/private/s3_default_meta_request.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_parallel_input_stream.h"
#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_util.h"
#include "aws/s3/private/s3express_credentials_provider_impl.h"
#include "aws/s3/s3express_credentials_provider.h"

#include <aws/auth/credentials.h>
#include <aws/common/assert.h>
#include <aws/common/atomics.h>
#include <aws/common/clock.h>
#include <aws/common/device_random.h>
#include <aws/common/json.h>
#include <aws/common/string.h>
#include <aws/common/system_info.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/proxy.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/retry_strategy.h>
#include <aws/io/socket.h>
#include <aws/io/stream.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#include <inttypes.h>
#include <math.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4232) /* function pointer to dll symbol */
#endif                              /* _MSC_VER */

struct aws_s3_meta_request_work {
    struct aws_linked_list_node node;
    struct aws_s3_meta_request *meta_request;
};

static const enum aws_log_level s_log_level_client_stats = AWS_LL_INFO;

/* max-requests-in-flight = ideal-num-connections * s_max_requests_multiplier */
static const uint32_t s_max_requests_multiplier = 4;

/* This is used to determine the ideal number of HTTP connections. Algorithm is roughly:
 * num-connections-max = throughput-target-gbps / s_throughput_per_connection_gbps
 *
 * Magic value based on: match results of the previous algorithm,
 * where throughput-target-gpbs of 100 resulted in 250 connections.
 *
 * TODO: Improve this algorithm (expect higher throughput for S3 Express,
 * expect lower throughput for small objects, etc)
 */
static const double s_throughput_per_connection_gbps = 100.0 / 250;

/* After throughput math, clamp the min/max number of connections */
const uint32_t g_min_num_connections = 10; /* Magic value based on: 10 was old behavior */

/**
 * Default part size is 8 MiB to reach the best performance from the experiments we had.
 * Default max part size is 5GiB as the server limit. Object size limit is 5TiB for now.
 *        max number of upload parts is 10000.
 * TODO Provide more information on other values.
 */
static const size_t s_default_part_size = 8 * 1024 * 1024;
static const uint64_t s_default_max_part_size = 5368709120ULL;
static const double s_default_throughput_target_gbps = 10.0;
static const uint32_t s_default_max_retries = 5;
static size_t s_dns_host_address_ttl_seconds = 5 * 60;

/* Default time until a connection is declared dead, while handling a request but seeing no activity.
 * 30 seconds mirrors the value currently used by the Java SDK. */
static const uint32_t s_default_throughput_failure_interval_seconds = 30;

/* Amount of time spent idling before trimming buffer. */
static const size_t s_buffer_pool_trim_time_offset_in_s = 5;

/* Interval for scheduling endpoints cleanup task. This is to trim endpoints with a zero reference
 * count. S3 closes the idle connections in ~5 seconds. */
static const uint32_t s_endpoints_cleanup_time_offset_in_s = 5;

/* Called when ref count is 0. */
static void s_s3_client_start_destroy(void *user_data);

/* Called by s_s3_client_process_work_default when all shutdown criteria has been met. */
static void s_s3_client_finish_destroy_default(struct aws_s3_client *client);

/* Called when the body streaming elg shutdown has completed. */
static void s_s3_client_body_streaming_elg_shutdown(void *user_data);

static void s_s3_client_create_connection_for_request(struct aws_s3_client *client, struct aws_s3_request *request);

static void s_s3_endpoints_cleanup_task(struct aws_task *task, void *arg, enum aws_task_status task_status);

/* Callback which handles the HTTP connection retrieved by acquire_http_connection. */
static void s_s3_client_on_acquire_http_connection(
    struct aws_http_connection *http_connection,
    int error_code,
    void *user_data);

static void s_s3_client_push_meta_request_synced(
    struct aws_s3_client *client,
    struct aws_s3_meta_request *meta_request);

/* Schedule task for processing work. (Calls the corresponding vtable function.) */
static void s_s3_client_schedule_process_work_synced(struct aws_s3_client *client);

/* Default implementation for scheduling processing of work. */
static void s_s3_client_schedule_process_work_synced_default(struct aws_s3_client *client);

/* Actual task function that processes work. */
static void s_s3_client_process_work_task(struct aws_task *task, void *arg, enum aws_task_status task_status);

static void s_s3_client_process_work_default(struct aws_s3_client *client);

static void s_s3_client_endpoint_shutdown_callback(struct aws_s3_client *client);

/* Default factory function for creating a meta request. */
static struct aws_s3_meta_request *s_s3_client_meta_request_factory_default(
    struct aws_s3_client *client,
    const struct aws_s3_meta_request_options *options);

static struct aws_s3_client_vtable s_s3_client_default_vtable = {
    .meta_request_factory = s_s3_client_meta_request_factory_default,
    .acquire_http_connection = aws_http_connection_manager_acquire_connection,
    .get_host_address_count = aws_host_resolver_get_host_address_count,
    .schedule_process_work_synced = s_s3_client_schedule_process_work_synced_default,
    .process_work = s_s3_client_process_work_default,
    .endpoint_shutdown_callback = s_s3_client_endpoint_shutdown_callback,
    .finish_destroy = s_s3_client_finish_destroy_default,
    .parallel_input_stream_new_from_file = aws_parallel_input_stream_new_from_file,
    .http_connection_make_request = aws_http_connection_make_request,
};

void aws_s3_set_dns_ttl(size_t ttl) {
    s_dns_host_address_ttl_seconds = ttl;
}

/* Returns the max number of connections allowed.
 *
 * When meta request is NULL, this will return the overall allowed number of connections.
 *
 * If meta_request is not NULL, this will give the max number of connections allowed for that meta request type on
 * that endpoint.
 */
uint32_t aws_s3_client_get_max_active_connections(
    struct aws_s3_client *client,
    struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(client);
    (void)meta_request;

    uint32_t max_active_connections = client->ideal_connection_count;

    if (client->max_active_connections_override > 0 &&
        client->max_active_connections_override < max_active_connections) {
        max_active_connections = client->max_active_connections_override;
    }

    return max_active_connections;
}

/* Returns the max number of requests allowed to be in memory */
uint32_t aws_s3_client_get_max_requests_in_flight(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);
    return aws_s3_client_get_max_active_connections(client, NULL) * s_max_requests_multiplier;
}

/* Returns the max number of requests that should be in preparation stage (ie: reading from a stream, being signed,
 * etc.) */
uint32_t aws_s3_client_get_max_requests_prepare(struct aws_s3_client *client) {
    return aws_s3_client_get_max_active_connections(client, NULL);
}

static uint32_t s_s3_client_get_num_requests_network_io(
    struct aws_s3_client *client,
    enum aws_s3_meta_request_type meta_request_type) {
    AWS_PRECONDITION(client);

    uint32_t num_requests_network_io = 0;

    if (meta_request_type == AWS_S3_META_REQUEST_TYPE_MAX) {
        for (uint32_t i = 0; i < AWS_S3_META_REQUEST_TYPE_MAX; ++i) {
            num_requests_network_io += (uint32_t)aws_atomic_load_int(&client->stats.num_requests_network_io[i]);
        }
    } else {
        num_requests_network_io =
            (uint32_t)aws_atomic_load_int(&client->stats.num_requests_network_io[meta_request_type]);
    }

    return num_requests_network_io;
}

void aws_s3_client_lock_synced_data(struct aws_s3_client *client) {
    aws_mutex_lock(&client->synced_data.lock);
}

void aws_s3_client_unlock_synced_data(struct aws_s3_client *client) {
    aws_mutex_unlock(&client->synced_data.lock);
}

static void s_s3express_provider_finish_destroy(void *user_data) {
    struct aws_s3_client *client = user_data;
    AWS_PRECONDITION(client);
    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);
        client->synced_data.s3express_provider_active = false;
        /* Schedule the work task to call s_s3_client_finish_destroy function if
         * everything cleaning up asynchronously has finished. */
        s_s3_client_schedule_process_work_synced(client);
        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */
}

struct aws_s3express_credentials_provider *s_s3express_provider_default_factory(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    aws_simple_completion_callback on_provider_shutdown_callback,
    void *shutdown_user_data,
    void *factory_user_data) {
    (void)factory_user_data;

    struct aws_s3express_credentials_provider_default_options options = {
        .client = client,
        .shutdown_complete_callback = on_provider_shutdown_callback,
        .shutdown_user_data = shutdown_user_data,
    };
    struct aws_s3express_credentials_provider *s3express_provider =
        aws_s3express_credentials_provider_new_default(allocator, &options);
    return s3express_provider;
}

struct aws_s3_client *aws_s3_client_new(
    struct aws_allocator *allocator,
    const struct aws_s3_client_config *client_config) {

    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(client_config);

    if (client_config->client_bootstrap == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Cannot create client from client_config; client_bootstrap provided in options is invalid.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    /* Cannot be less than zero.  If zero, use default. */
    if (client_config->throughput_target_gbps < 0.0) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Cannot create client from client_config; throughput_target_gbps cannot less than or equal to 0.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (client_config->signing_config == NULL) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "Cannot create client from client_config; signing_config is required.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (client_config->signing_config->credentials == NULL &&
        client_config->signing_config->credentials_provider == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Cannot create client from client_config; Invalid signing_config provided, either credentials or "
            "credentials provider has to be set.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (!client_config->enable_s3express &&
        client_config->signing_config->algorithm == AWS_SIGNING_ALGORITHM_V4_S3EXPRESS) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Cannot create client from client_config; Client config is set use S3 Express signing, but S3 Express "
            "support is "
            "not configured.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

#ifdef BYO_CRYPTO
    if (client_config->tls_mode == AWS_MR_TLS_ENABLED && client_config->tls_connection_options == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Cannot create client from client_config; when using BYO_CRYPTO, tls_connection_options can not be "
            "NULL when TLS is enabled.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }
#endif

    struct aws_s3_client *client = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_client));

    client->allocator = allocator;

    size_t mem_limit = 0;
    if (client_config->memory_limit_in_bytes == 0) {
#if SIZE_BITS == 32
        if (client_config->throughput_target_gbps > 25.0) {
            mem_limit = GB_TO_BYTES(2);
        } else {
            mem_limit = GB_TO_BYTES(1);
        }
#else
        if (client_config->throughput_target_gbps > 75.0) {
            mem_limit = GB_TO_BYTES(8);
        } else if (client_config->throughput_target_gbps > 25.0) {
            mem_limit = GB_TO_BYTES(4);
        } else {
            mem_limit = GB_TO_BYTES(2);
        }
#endif
    } else {
        // cap memory limit to SIZE_MAX
        if (client_config->memory_limit_in_bytes > SIZE_MAX) {
            mem_limit = SIZE_MAX;
        } else {
            mem_limit = (size_t)client_config->memory_limit_in_bytes;
        }
    }

    size_t part_size = s_default_part_size;
    if (client_config->part_size != 0) {
        if (client_config->part_size > SIZE_MAX) {
            part_size = SIZE_MAX;
        } else {
            part_size = (size_t)client_config->part_size;
        }
    }

    client->buffer_pool = aws_s3_buffer_pool_new(allocator, part_size, mem_limit);

    if (client->buffer_pool == NULL) {
        goto on_error;
    }

    struct aws_s3_buffer_pool_usage_stats pool_usage = aws_s3_buffer_pool_get_usage(client->buffer_pool);

    if (client_config->max_part_size > pool_usage.mem_limit) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Cannot create client from client_config; configured max part size should not exceed memory limit."
            "size.");
        aws_raise_error(AWS_ERROR_S3_INVALID_MEMORY_LIMIT_CONFIG);
        goto on_error;
    }

    client->vtable = &s_s3_client_default_vtable;

    aws_ref_count_init(&client->ref_count, client, (aws_simple_completion_callback *)s_s3_client_start_destroy);

    if (aws_mutex_init(&client->synced_data.lock) != AWS_OP_SUCCESS) {
        goto on_error;
    }

    aws_linked_list_init(&client->synced_data.pending_meta_request_work);
    aws_linked_list_init(&client->synced_data.prepared_requests);

    aws_linked_list_init(&client->threaded_data.meta_requests);
    aws_linked_list_init(&client->threaded_data.request_queue);

    aws_atomic_init_int(&client->stats.num_requests_in_flight, 0);

    for (uint32_t i = 0; i < (uint32_t)AWS_S3_META_REQUEST_TYPE_MAX; ++i) {
        aws_atomic_init_int(&client->stats.num_requests_network_io[i], 0);
    }

    aws_atomic_init_int(&client->stats.num_requests_stream_queued_waiting, 0);
    aws_atomic_init_int(&client->stats.num_requests_streaming_response, 0);

    *((uint32_t *)&client->max_active_connections_override) = client_config->max_active_connections_override;

    /* Store our client bootstrap. */
    client->client_bootstrap = aws_client_bootstrap_acquire(client_config->client_bootstrap);

    struct aws_event_loop_group *event_loop_group = client_config->client_bootstrap->event_loop_group;
    aws_event_loop_group_acquire(event_loop_group);

    client->process_work_event_loop = aws_event_loop_group_get_next_loop(event_loop_group);

    /* Make a copy of the region string. */
    client->region = aws_string_new_from_array(allocator, client_config->region.ptr, client_config->region.len);

    *((size_t *)&client->part_size) = part_size;

    if (client_config->max_part_size != 0) {
        *((uint64_t *)&client->max_part_size) = client_config->max_part_size;
    } else {
        *((uint64_t *)&client->max_part_size) = s_default_max_part_size;
    }

    if (client_config->max_part_size > pool_usage.mem_limit) {
        *((uint64_t *)&client->max_part_size) = pool_usage.mem_limit;
    }

    if (client->max_part_size > SIZE_MAX) {
        /* For the 32bit max part size to be SIZE_MAX */
        *((uint64_t *)&client->max_part_size) = SIZE_MAX;
    }

    if (client_config->multipart_upload_threshold != 0) {
        *((uint64_t *)&client->multipart_upload_threshold) = client_config->multipart_upload_threshold;
    } else {
        *((uint64_t *)&client->multipart_upload_threshold) =
            part_size > g_s3_min_upload_part_size ? part_size : g_s3_min_upload_part_size;
    }

    if (client_config->max_part_size < client_config->part_size) {
        *((uint64_t *)&client_config->max_part_size) = client_config->part_size;
    }

    client->connect_timeout_ms = client_config->connect_timeout_ms;
    if (client_config->proxy_ev_settings) {
        client->proxy_ev_settings = aws_mem_calloc(allocator, 1, sizeof(struct proxy_env_var_settings));
        *client->proxy_ev_settings = *client_config->proxy_ev_settings;

        if (client_config->proxy_ev_settings->tls_options) {
            client->proxy_ev_tls_options = aws_mem_calloc(allocator, 1, sizeof(struct aws_tls_connection_options));
            if (aws_tls_connection_options_copy(client->proxy_ev_tls_options, client->proxy_ev_settings->tls_options)) {
                goto on_error;
            }
            client->proxy_ev_settings->tls_options = client->proxy_ev_tls_options;
        }
    }

    if (client_config->tcp_keep_alive_options) {
        client->tcp_keep_alive_options = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_tcp_keep_alive_options));
        *client->tcp_keep_alive_options = *client_config->tcp_keep_alive_options;
    }

    if (client_config->monitoring_options) {
        client->monitoring_options = *client_config->monitoring_options;
    } else {
        client->monitoring_options.minimum_throughput_bytes_per_second = 1;
        client->monitoring_options.allowable_throughput_failure_interval_seconds =
            s_default_throughput_failure_interval_seconds;
    }

    if (client_config->tls_mode == AWS_MR_TLS_ENABLED) {
        client->tls_connection_options =
            aws_mem_calloc(client->allocator, 1, sizeof(struct aws_tls_connection_options));

        if (client_config->tls_connection_options != NULL) {
            aws_tls_connection_options_copy(client->tls_connection_options, client_config->tls_connection_options);
        } else {
#ifdef BYO_CRYPTO
            AWS_FATAL_ASSERT(false);
            goto on_error;
#else
            struct aws_tls_ctx_options default_tls_ctx_options;
            AWS_ZERO_STRUCT(default_tls_ctx_options);

            aws_tls_ctx_options_init_default_client(&default_tls_ctx_options, allocator);

            struct aws_tls_ctx *default_tls_ctx = aws_tls_client_ctx_new(allocator, &default_tls_ctx_options);
            if (default_tls_ctx == NULL) {
                goto on_error;
            }

            aws_tls_connection_options_init_from_ctx(client->tls_connection_options, default_tls_ctx);

            aws_tls_ctx_release(default_tls_ctx);
            aws_tls_ctx_options_clean_up(&default_tls_ctx_options);
#endif
        }
    }

    if (client_config->proxy_options) {
        client->proxy_config = aws_http_proxy_config_new_from_proxy_options_with_tls_info(
            allocator, client_config->proxy_options, client_config->tls_mode == AWS_MR_TLS_ENABLED);
        if (client->proxy_config == NULL) {
            goto on_error;
        }
    }

    client->num_network_interface_names = client_config->num_network_interface_names;
    if (client_config->num_network_interface_names > 0) {
        AWS_LOGF_DEBUG(
            AWS_LS_S3_CLIENT,
            "id=%p Client received network interface names array with length %zu.",
            (void *)client,
            client->num_network_interface_names);
        aws_array_list_init_dynamic(
            &client->network_interface_names,
            client->allocator,
            client_config->num_network_interface_names,
            sizeof(struct aws_string *));
        client->network_interface_names_cursor_array = aws_mem_calloc(
            client->allocator, client_config->num_network_interface_names, sizeof(struct aws_byte_cursor));
        for (size_t i = 0; i < client_config->num_network_interface_names; i++) {
            struct aws_byte_cursor interface_name = client_config->network_interface_names_array[i];
            struct aws_string *interface_name_str = aws_string_new_from_cursor(client->allocator, &interface_name);
            aws_array_list_push_back(&client->network_interface_names, &interface_name_str);
            if (aws_is_network_interface_name_valid(aws_string_c_str(interface_name_str)) == false) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_CLIENT,
                    "id=%p network_interface_names_array[%zu]=" PRInSTR " is not valid.",
                    (void *)client,
                    i,
                    AWS_BYTE_CURSOR_PRI(interface_name));
                aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                goto on_error;
            }
            client->network_interface_names_cursor_array[i] = aws_byte_cursor_from_string(interface_name_str);
            AWS_LOGF_DEBUG(
                AWS_LS_S3_CLIENT,
                "id=%p network_interface_names_array[%zu]=" PRInSTR "",
                (void *)client,
                i,
                AWS_BYTE_CURSOR_PRI(client->network_interface_names_cursor_array[i]));
        }
    }

    /* Set up body streaming ELG */
    {
        uint16_t num_event_loops =
            (uint16_t)aws_event_loop_group_get_loop_count(client->client_bootstrap->event_loop_group);
        uint16_t num_streaming_threads = num_event_loops;

        if (num_streaming_threads < 1) {
            num_streaming_threads = 1;
        }

        struct aws_shutdown_callback_options body_streaming_elg_shutdown_options = {
            .shutdown_callback_fn = s_s3_client_body_streaming_elg_shutdown,
            .shutdown_callback_user_data = client,
        };

        client->body_streaming_elg = aws_event_loop_group_new_default(
            client->allocator, num_streaming_threads, &body_streaming_elg_shutdown_options);

        if (!client->body_streaming_elg) {
            /* Fail to create elg, we should fail the call */
            goto on_error;
        }
        client->synced_data.body_streaming_elg_allocated = true;
    }
    /* Setup cannot fail after this point. */

    if (client_config->throughput_target_gbps > 0.0) {
        *((double *)&client->throughput_target_gbps) = client_config->throughput_target_gbps;
    } else {
        *((double *)&client->throughput_target_gbps) = s_default_throughput_target_gbps;
    }

    *((enum aws_s3_meta_request_compute_content_md5 *)&client->compute_content_md5) =
        client_config->compute_content_md5;

    /* Determine how many connections are ideal by dividing target-throughput by throughput-per-connection. */
    {
        double ideal_connection_count_double = client->throughput_target_gbps / s_throughput_per_connection_gbps;
        /* round up and clamp */
        ideal_connection_count_double = ceil(ideal_connection_count_double);
        ideal_connection_count_double = aws_max_double(g_min_num_connections, ideal_connection_count_double);
        ideal_connection_count_double = aws_min_double(UINT32_MAX, ideal_connection_count_double);
        *(uint32_t *)&client->ideal_connection_count = (uint32_t)ideal_connection_count_double;
    }

    client->cached_signing_config = aws_cached_signing_config_new(client, client_config->signing_config);
    if (client_config->enable_s3express) {
        if (client_config->s3express_provider_override_factory) {
            client->s3express_provider_factory = client_config->s3express_provider_override_factory;
            client->factory_user_data = client_config->factory_user_data;
        } else {
            client->s3express_provider_factory = s_s3express_provider_default_factory;
        }
    }

    client->synced_data.active = true;

    if (client_config->retry_strategy != NULL) {
        aws_retry_strategy_acquire(client_config->retry_strategy);
        client->retry_strategy = client_config->retry_strategy;
    } else {
        struct aws_exponential_backoff_retry_options backoff_retry_options = {
            .el_group = client_config->client_bootstrap->event_loop_group,
            .max_retries = s_default_max_retries,
        };

        struct aws_standard_retry_options retry_options = {
            .backoff_retry_options = backoff_retry_options,
        };

        client->retry_strategy = aws_retry_strategy_new_standard(allocator, &retry_options);
    }

    aws_hash_table_init(
        &client->synced_data.endpoints,
        client->allocator,
        10,
        aws_hash_string,
        aws_hash_callback_string_eq,
        aws_hash_callback_string_destroy,
        NULL);
    aws_task_init(
        &client->synced_data.endpoints_cleanup_task, s_s3_endpoints_cleanup_task, client, "s3_endpoints_cleanup_task");

    /* Initialize shutdown options and tracking. */
    client->shutdown_callback = client_config->shutdown_callback;
    client->shutdown_callback_user_data = client_config->shutdown_callback_user_data;

    *((bool *)&client->enable_read_backpressure) = client_config->enable_read_backpressure;
    *((size_t *)&client->initial_read_window) = client_config->initial_read_window;

    return client;

on_error:
    aws_string_destroy(client->region);

    if (client->tls_connection_options) {
        aws_tls_connection_options_clean_up(client->tls_connection_options);
        aws_mem_release(client->allocator, client->tls_connection_options);
        client->tls_connection_options = NULL;
    }
    if (client->proxy_config) {
        aws_http_proxy_config_destroy(client->proxy_config);
    }
    if (client->proxy_ev_tls_options) {
        aws_tls_connection_options_clean_up(client->proxy_ev_tls_options);
        aws_mem_release(client->allocator, client->proxy_ev_tls_options);
        client->proxy_ev_settings->tls_options = NULL;
    }
    aws_mem_release(client->allocator, client->proxy_ev_settings);
    aws_mem_release(client->allocator, client->tcp_keep_alive_options);

    if (client->client_bootstrap != NULL) {
        aws_event_loop_group_release(client->client_bootstrap->event_loop_group);
    }
    aws_client_bootstrap_release(client->client_bootstrap);
    aws_mutex_clean_up(&client->synced_data.lock);

    aws_mem_release(client->allocator, client->network_interface_names_cursor_array);
    for (size_t i = 0; i < aws_array_list_length(&client->network_interface_names); i++) {
        struct aws_string *interface_name = NULL;
        aws_array_list_get_at(&client->network_interface_names, &interface_name, i);
        aws_string_destroy(interface_name);
    }

    aws_array_list_clean_up(&client->network_interface_names);
    aws_s3_buffer_pool_destroy(client->buffer_pool);

    aws_mem_release(client->allocator, client);
    return NULL;
}

struct aws_s3_client *aws_s3_client_acquire(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    aws_ref_count_acquire(&client->ref_count);
    return client;
}

struct aws_s3_client *aws_s3_client_release(struct aws_s3_client *client) {
    if (client != NULL) {
        aws_ref_count_release(&client->ref_count);
    }

    return NULL;
}

static void s_s3_client_start_destroy(void *user_data) {
    struct aws_s3_client *client = user_data;
    AWS_PRECONDITION(client);

    AWS_LOGF_DEBUG(AWS_LS_S3_CLIENT, "id=%p Client starting destruction.", (void *)client);

    struct aws_linked_list local_vip_list;
    aws_linked_list_init(&local_vip_list);

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);

        client->synced_data.active = false;
        if (!client->synced_data.endpoints_cleanup_task_scheduled) {
            client->synced_data.endpoints_cleanup_task_scheduled = true;
            aws_event_loop_schedule_task_now(
                client->process_work_event_loop, &client->synced_data.endpoints_cleanup_task);
        }
        /* Prevent the client from cleaning up in between the mutex unlock/re-lock below.*/
        client->synced_data.start_destroy_executing = true;

        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */

    aws_event_loop_group_release(client->body_streaming_elg);
    client->body_streaming_elg = NULL;
    aws_s3express_credentials_provider_release(client->s3express_provider);

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);
        client->synced_data.start_destroy_executing = false;

        /* Schedule the work task to clean up outstanding connections and to call s_s3_client_finish_destroy function if
         * everything cleaning up asynchronously has finished.  */
        s_s3_client_schedule_process_work_synced(client);
        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */
}

static void s_s3_client_finish_destroy_default(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    AWS_LOGF_DEBUG(AWS_LS_S3_CLIENT, "id=%p Client finishing destruction.", (void *)client);

    if (client->threaded_data.trim_buffer_pool_task_scheduled) {
        aws_event_loop_cancel_task(client->process_work_event_loop, &client->synced_data.trim_buffer_pool_task);
    }

    aws_string_destroy(client->region);
    client->region = NULL;

    if (client->tls_connection_options) {
        aws_tls_connection_options_clean_up(client->tls_connection_options);
        aws_mem_release(client->allocator, client->tls_connection_options);
        client->tls_connection_options = NULL;
    }

    if (client->proxy_config) {
        aws_http_proxy_config_destroy(client->proxy_config);
    }

    if (client->proxy_ev_tls_options) {
        aws_tls_connection_options_clean_up(client->proxy_ev_tls_options);
        aws_mem_release(client->allocator, client->proxy_ev_tls_options);
        client->proxy_ev_settings->tls_options = NULL;
    }
    aws_mem_release(client->allocator, client->proxy_ev_settings);
    aws_mem_release(client->allocator, client->tcp_keep_alive_options);

    aws_mutex_clean_up(&client->synced_data.lock);

    AWS_ASSERT(aws_linked_list_empty(&client->synced_data.pending_meta_request_work));
    AWS_ASSERT(aws_linked_list_empty(&client->threaded_data.meta_requests));
    aws_hash_table_clean_up(&client->synced_data.endpoints);

    aws_retry_strategy_release(client->retry_strategy);

    aws_event_loop_group_release(client->client_bootstrap->event_loop_group);

    aws_client_bootstrap_release(client->client_bootstrap);
    aws_cached_signing_config_destroy(client->cached_signing_config);

    aws_s3_client_shutdown_complete_callback_fn *shutdown_callback = client->shutdown_callback;
    void *shutdown_user_data = client->shutdown_callback_user_data;

    aws_s3_buffer_pool_destroy(client->buffer_pool);

    aws_mem_release(client->allocator, client->network_interface_names_cursor_array);
    for (size_t i = 0; i < client->num_network_interface_names; i++) {
        struct aws_string *interface_name = NULL;
        aws_array_list_get_at(&client->network_interface_names, &interface_name, i);
        aws_string_destroy(interface_name);
    }
    aws_array_list_clean_up(&client->network_interface_names);

    aws_mem_release(client->allocator, client);
    client = NULL;

    if (shutdown_callback != NULL) {
        shutdown_callback(shutdown_user_data);
    }
}

static void s_s3_client_body_streaming_elg_shutdown(void *user_data) {
    struct aws_s3_client *client = user_data;
    AWS_PRECONDITION(client);

    AWS_LOGF_DEBUG(AWS_LS_S3_CLIENT, "id=%p Client body streaming ELG shutdown.", (void *)client);

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);
        client->synced_data.body_streaming_elg_allocated = false;
        s_s3_client_schedule_process_work_synced(client);
        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */
}

uint32_t aws_s3_client_queue_requests_threaded(
    struct aws_s3_client *client,
    struct aws_linked_list *request_list,
    bool queue_front) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(request_list);
    if (aws_linked_list_empty(request_list)) {
        return 0;
    }

    uint32_t request_list_size = 0;

    for (struct aws_linked_list_node *node = aws_linked_list_begin(request_list);
         node != aws_linked_list_end(request_list);
         node = aws_linked_list_next(node)) {
        ++request_list_size;
    }

    if (queue_front) {
        aws_linked_list_move_all_front(&client->threaded_data.request_queue, request_list);
    } else {
        aws_linked_list_move_all_back(&client->threaded_data.request_queue, request_list);
    }

    client->threaded_data.request_queue_size += request_list_size;
    return request_list_size;
}

struct aws_s3_request *aws_s3_client_dequeue_request_threaded(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    if (aws_linked_list_empty(&client->threaded_data.request_queue)) {
        return NULL;
    }

    struct aws_linked_list_node *request_node = aws_linked_list_pop_front(&client->threaded_data.request_queue);
    struct aws_s3_request *request = AWS_CONTAINER_OF(request_node, struct aws_s3_request, node);

    --client->threaded_data.request_queue_size;

    return request;
}

/*
 * There is currently some overlap between user provided Host header and endpoint
 * override. This function handles the corner cases for when either or both are provided.
 */
int s_apply_endpoint_override(
    const struct aws_s3_client *client,
    struct aws_http_headers *message_headers,
    const struct aws_uri *endpoint) {
    AWS_PRECONDITION(message_headers);

    const struct aws_byte_cursor *endpoint_authority = endpoint == NULL ? NULL : aws_uri_authority(endpoint);

    if (!aws_http_headers_has(message_headers, g_host_header_name)) {
        if (endpoint_authority == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_CLIENT,
                "id=%p Cannot create meta s3 request; message provided in options does not have either 'Host' header "
                "set or endpoint override.",
                (void *)client);
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        if (aws_http_headers_set(message_headers, g_host_header_name, *endpoint_authority)) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_CLIENT,
                "id=%p Cannot create meta s3 request; failed to set 'Host' header based on endpoint override.",
                (void *)client);
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
    }

    struct aws_byte_cursor host_value;
    AWS_FATAL_ASSERT(aws_http_headers_get(message_headers, g_host_header_name, &host_value) == AWS_OP_SUCCESS);

    if (endpoint_authority != NULL && !aws_byte_cursor_eq(&host_value, endpoint_authority)) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Cannot create meta s3 request; host header value " PRInSTR
            " does not match endpoint override " PRInSTR,
            (void *)client,
            AWS_BYTE_CURSOR_PRI(host_value),
            AWS_BYTE_CURSOR_PRI(*endpoint_authority));
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return AWS_OP_SUCCESS;
}

/* Public facing make-meta-request function. */
struct aws_s3_meta_request *aws_s3_client_make_meta_request(
    struct aws_s3_client *client,
    const struct aws_s3_meta_request_options *options) {

    AWS_LOGF_INFO(AWS_LS_S3_CLIENT, "id=%p Initiating making of meta request", (void *)client);

    AWS_PRECONDITION(client);
    AWS_PRECONDITION(client->vtable);
    AWS_PRECONDITION(client->vtable->meta_request_factory);
    AWS_PRECONDITION(options);

    bool use_s3express_signing = false;
    if (options->signing_config != NULL) {
        use_s3express_signing = options->signing_config->algorithm == AWS_SIGNING_ALGORITHM_V4_S3EXPRESS;
    } else if (client->cached_signing_config) {
        use_s3express_signing = client->cached_signing_config->config.algorithm == AWS_SIGNING_ALGORITHM_V4_S3EXPRESS;
    }

    if (options->type >= AWS_S3_META_REQUEST_TYPE_MAX) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Cannot create meta s3 request; invalid meta request type specified.",
            (void *)client);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (options->message == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Cannot create meta s3 request; message provided in options is invalid.",
            (void *)client);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (use_s3express_signing && client->s3express_provider_factory == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Cannot create meta s3 request; client doesn't support S3 Express signing.",
            (void *)client);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_headers *message_headers = aws_http_message_get_headers(options->message);

    if (message_headers == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Cannot create meta s3 request; message provided in options does not contain headers.",
            (void *)client);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (options->checksum_config) {
        if (options->checksum_config->location == AWS_SCL_TRAILER) {
            struct aws_http_headers *headers = aws_http_message_get_headers(options->message);
            struct aws_byte_cursor existing_encoding;
            AWS_ZERO_STRUCT(existing_encoding);
            if (aws_http_headers_get(headers, g_content_encoding_header_name, &existing_encoding) == AWS_OP_SUCCESS) {
                if (aws_byte_cursor_find_exact(&existing_encoding, &g_content_encoding_header_aws_chunked, NULL) ==
                    AWS_OP_SUCCESS) {
                    AWS_LOGF_ERROR(
                        AWS_LS_S3_CLIENT,
                        "id=%p Cannot create meta s3 request; for trailer checksum, the original request cannot be "
                        "aws-chunked encoding. The client will encode the request instead.",
                        (void *)client);
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    return NULL;
                }
            }
        }

        if (options->checksum_config->location != AWS_SCL_NONE &&
            options->checksum_config->checksum_algorithm == AWS_SCA_NONE) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_CLIENT,
                "id=%p Cannot create meta s3 request; checksum location is set, but no checksum algorithm selected.",
                (void *)client);
            aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            return NULL;
        }
    }

    if (s_apply_endpoint_override(client, message_headers, options->endpoint)) {
        return NULL;
    }

    struct aws_byte_cursor host_header_value;
    /* The Host header must be set from s_apply_endpoint_override, if not errored out */
    AWS_FATAL_ASSERT(aws_http_headers_get(message_headers, g_host_header_name, &host_header_value) == AWS_OP_SUCCESS);

    bool is_https = true;
    uint32_t port = 0;

    if (options->endpoint != NULL) {
        struct aws_byte_cursor https_scheme = aws_byte_cursor_from_c_str("https");
        struct aws_byte_cursor http_scheme = aws_byte_cursor_from_c_str("http");

        const struct aws_byte_cursor *scheme = aws_uri_scheme(options->endpoint);

        is_https = aws_byte_cursor_eq_ignore_case(scheme, &https_scheme);

        if (!is_https && !aws_byte_cursor_eq_ignore_case(scheme, &http_scheme)) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_CLIENT,
                "id=%p Cannot create meta s3 request; unexpected scheme '" PRInSTR "' in endpoint override.",
                (void *)client,
                AWS_BYTE_CURSOR_PRI(*scheme));
            aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            return NULL;
        }

        port = aws_uri_port(options->endpoint);
    }

    struct aws_s3_meta_request *meta_request = client->vtable->meta_request_factory(client, options);

    if (meta_request == NULL) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "id=%p: Could not create new meta request.", (void *)client);
        return NULL;
    }

    bool error_occurred = false;

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);

        if (use_s3express_signing && !client->synced_data.s3express_provider_active) {

            AWS_LOGF_TRACE(AWS_LS_S3_CLIENT, "id=%p Create S3 Express provider for the client.", (void *)client);
            /**
             * Invoke the factory within the lock. We WARNED people uses their own factory to not use ANY client related
             * api during the factory.
             *
             * We cannot just release the lock and invoke the factory, because it can lead to the other request assume
             * the provider is active, and not waiting for the provider to be created. And lead to unexpected behavior.
             */
            client->s3express_provider = client->s3express_provider_factory(
                client->allocator, client, s_s3express_provider_finish_destroy, client, client->factory_user_data);

            /* Provider is related to client, we don't need to clean it up if meta request failed. But, if provider
             * failed to be created, let's bail out earlier. */
            if (!client->s3express_provider) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_CLIENT,
                    "id=%p Failed to create S3 Express provider for client due to error %d (%s)",
                    (void *)client,
                    aws_last_error_or_unknown(),
                    aws_error_str(aws_last_error_or_unknown()));
                error_occurred = true;
                goto unlock;
            }
            client->synced_data.s3express_provider_active = true;
        }

        struct aws_string *endpoint_host_name = NULL;

        if (options->endpoint != NULL) {
            endpoint_host_name = aws_string_new_from_cursor(client->allocator, aws_uri_host_name(options->endpoint));
        } else {
            struct aws_uri host_uri;
            if (aws_uri_init_parse(&host_uri, client->allocator, &host_header_value)) {
                error_occurred = true;
                goto unlock;
            }

            endpoint_host_name = aws_string_new_from_cursor(client->allocator, aws_uri_host_name(&host_uri));
            aws_uri_clean_up(&host_uri);
        }

        struct aws_s3_endpoint *endpoint = NULL;
        struct aws_hash_element *endpoint_hash_element = NULL;

        if (use_s3express_signing) {
            meta_request->s3express_session_host = aws_string_new_from_string(client->allocator, endpoint_host_name);
        }

        int was_created = 0;
        if (aws_hash_table_create(
                &client->synced_data.endpoints, endpoint_host_name, &endpoint_hash_element, &was_created)) {
            aws_string_destroy(endpoint_host_name);
            error_occurred = true;
            goto unlock;
        }

        if (was_created) {
            struct aws_s3_endpoint_options endpoint_options = {
                .host_name = endpoint_host_name,
                .client_bootstrap = client->client_bootstrap,
                .tls_connection_options = is_https ? client->tls_connection_options : NULL,
                .dns_host_address_ttl_seconds = s_dns_host_address_ttl_seconds,
                .client = client,
                .max_connections = aws_s3_client_get_max_active_connections(client, NULL),
                .port = port,
                .proxy_config = client->proxy_config,
                .proxy_ev_settings = client->proxy_ev_settings,
                .connect_timeout_ms = client->connect_timeout_ms,
                .tcp_keep_alive_options = client->tcp_keep_alive_options,
                .monitoring_options = &client->monitoring_options,
                .network_interface_names_array = client->network_interface_names_cursor_array,
                .num_network_interface_names = client->num_network_interface_names,
            };

            endpoint = aws_s3_endpoint_new(client->allocator, &endpoint_options);

            if (endpoint == NULL) {
                aws_hash_table_remove(&client->synced_data.endpoints, endpoint_host_name, NULL, NULL);
                aws_string_destroy(endpoint_host_name);
                error_occurred = true;
                goto unlock;
            }
            endpoint_hash_element->value = endpoint;
            ++client->synced_data.num_endpoints_allocated;
        } else {
            endpoint = endpoint_hash_element->value;

            aws_s3_endpoint_acquire(endpoint, true /*already_holding_lock*/);

            aws_string_destroy(endpoint_host_name);
            endpoint_host_name = NULL;
        }

        meta_request->endpoint = endpoint;
        /**
         * shutdown_callback must be the last thing that gets set on the meta_request so that we don’t return NULL and
         * trigger the shutdown_callback.
         */
        meta_request->shutdown_callback = options->shutdown_callback;

        s_s3_client_push_meta_request_synced(client, meta_request);
        s_s3_client_schedule_process_work_synced(client);

    unlock:
        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */

    if (error_occurred) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Could not create meta request due to error %d (%s)",
            (void *)client,
            aws_last_error(),
            aws_error_str(aws_last_error()));

        meta_request = aws_s3_meta_request_release(meta_request);
    } else {
        AWS_LOGF_INFO(AWS_LS_S3_CLIENT, "id=%p: Created meta request %p", (void *)client, (void *)meta_request);
    }

    return meta_request;
}

static void s_s3_client_endpoint_shutdown_callback(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);
        --client->synced_data.num_endpoints_allocated;
        s_s3_client_schedule_process_work_synced(client);
        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */
}

static struct aws_s3_meta_request *s_s3_client_meta_request_factory_default(
    struct aws_s3_client *client,
    const struct aws_s3_meta_request_options *options) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(options);

    const struct aws_http_headers *initial_message_headers = aws_http_message_get_headers(options->message);
    AWS_ASSERT(initial_message_headers);

    uint64_t content_length = 0;
    struct aws_byte_cursor content_length_cursor;
    bool content_length_found = false;

    if (!aws_http_headers_get(initial_message_headers, g_content_length_header_name, &content_length_cursor)) {
        if (aws_byte_cursor_utf8_parse_u64(content_length_cursor, &content_length)) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "Could not parse Content-Length header. header value is:" PRInSTR "",
                AWS_BYTE_CURSOR_PRI(content_length_cursor));
            aws_raise_error(AWS_ERROR_S3_INVALID_CONTENT_LENGTH_HEADER);
            return NULL;
        }
        content_length_found = true;
    }

    /* There are multiple ways to pass the body in, ensure only 1 was used */
    int body_source_count = 0;
    if (aws_http_message_get_body_stream(options->message) != NULL) {
        ++body_source_count;
    }
    if (options->send_filepath.len > 0) {
        ++body_source_count;
    }
    if (options->send_using_async_writes == true) {
        if (options->type != AWS_S3_META_REQUEST_TYPE_PUT_OBJECT) {
            /* TODO: we could support async-writes for DEFAULT type too, just takes work & testing */
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "Could not create meta request."
                "send-using-data-writes can only be used with auto-ranged-put.");
            aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            return NULL;
        }
        if (content_length_found) {
            /* TODO: we could support async-writes with content-length, just takes work & testing */
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "Could not create meta request."
                "send-using-data-writes can only be used when Content-Length is unknown.");
            aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            return NULL;
        }
        ++body_source_count;
    }
    if (options->send_async_stream != NULL) {
        ++body_source_count;
    }
    if (body_source_count > 1) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create meta request."
            " More than one data source is set (filepath, async stream, body stream, data writes).");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }
    size_t part_size = client->part_size;
    if (options->part_size != 0) {
        if (options->part_size > SIZE_MAX) {
            part_size = SIZE_MAX;
        } else {
            part_size = (size_t)options->part_size;
        }
    }

    /* Call the appropriate meta-request new function. */
    switch (options->type) {
        case AWS_S3_META_REQUEST_TYPE_GET_OBJECT: {
            struct aws_byte_cursor path_and_query;

            if (aws_http_message_get_request_path(options->message, &path_and_query) == AWS_OP_SUCCESS) {
                /* If the initial request already has partNumber, the request is not
                 * splittable(?). Treat it as a Default request.
                 * TODO: Still need tests to verify that the request of a part is
                 * splittable or not */
                struct aws_byte_cursor sub_string;
                AWS_ZERO_STRUCT(sub_string);
                /* The first split on '?' for path and query is path, the second is query */
                if (aws_byte_cursor_next_split(&path_and_query, '?', &sub_string) == true) {
                    aws_byte_cursor_next_split(&path_and_query, '?', &sub_string);
                    struct aws_uri_param param;
                    AWS_ZERO_STRUCT(param);
                    struct aws_byte_cursor part_number_query_str = aws_byte_cursor_from_c_str("partNumber");
                    while (aws_query_string_next_param(sub_string, &param)) {
                        if (aws_byte_cursor_eq(&param.key, &part_number_query_str)) {
                            return aws_s3_meta_request_default_new(
                                client->allocator,
                                client,
                                AWS_S3_REQUEST_TYPE_GET_OBJECT,
                                content_length,
                                false /*should_compute_content_md5*/,
                                options);
                        }
                    }
                }
            }
            return aws_s3_meta_request_auto_ranged_get_new(client->allocator, client, part_size, options);
        }
        case AWS_S3_META_REQUEST_TYPE_PUT_OBJECT: {
            if (body_source_count == 0) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "Could not create auto-ranged-put meta request."
                    " Body must be set via filepath, async stream, or body stream.");
                aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                return NULL;
            }

            if (options->resume_token == NULL) {
                uint64_t client_max_part_size = client->max_part_size;

                if (part_size < g_s3_min_upload_part_size) {
                    AWS_LOGF_WARN(
                        AWS_LS_S3_META_REQUEST,
                        "Config part size of %" PRIu64 " is less than the minimum upload part size of %" PRIu64
                        ". Using to the minimum part-size for upload.",
                        (uint64_t)part_size,
                        (uint64_t)g_s3_min_upload_part_size);

                    part_size = g_s3_min_upload_part_size;
                }

                if (client_max_part_size < (uint64_t)g_s3_min_upload_part_size) {
                    AWS_LOGF_WARN(
                        AWS_LS_S3_META_REQUEST,
                        "Client config max part size of %" PRIu64
                        " is less than the minimum upload part size of %" PRIu64
                        ". Clamping to the minimum part-size for upload.",
                        (uint64_t)client_max_part_size,
                        (uint64_t)g_s3_min_upload_part_size);

                    client_max_part_size = (uint64_t)g_s3_min_upload_part_size;
                }

                uint32_t num_parts = 0;
                if (content_length_found) {
                    size_t out_part_size = 0;
                    if (aws_s3_calculate_optimal_mpu_part_size_and_num_parts(
                            content_length, part_size, client_max_part_size, &out_part_size, &num_parts)) {
                        return NULL;
                    }
                    part_size = out_part_size;
                }
                if (part_size != options->part_size && part_size != client->part_size) {
                    AWS_LOGF_DEBUG(
                        AWS_LS_S3_META_REQUEST,
                        "The multipart upload part size has been adjusted to %" PRIu64 "",
                        (uint64_t)part_size);
                }

                /* Default to client level setting */
                uint64_t multipart_upload_threshold = client->multipart_upload_threshold;
                if (options->multipart_upload_threshold != 0) {
                    /* If the threshold is set for the meta request, use it */
                    multipart_upload_threshold = options->multipart_upload_threshold;
                } else if (options->part_size != 0) {
                    /* If the threshold is not set, but the part size is set for the meta request, use it */
                    multipart_upload_threshold = part_size;
                }

                if (content_length_found && content_length <= multipart_upload_threshold) {
                    return aws_s3_meta_request_default_new(
                        client->allocator,
                        client,
                        AWS_S3_REQUEST_TYPE_PUT_OBJECT,
                        content_length,
                        client->compute_content_md5 == AWS_MR_CONTENT_MD5_ENABLED &&
                            !aws_http_headers_has(initial_message_headers, g_content_md5_header_name),
                        options);
                }
                return aws_s3_meta_request_auto_ranged_put_new(
                    client->allocator, client, part_size, content_length_found, content_length, num_parts, options);
            } else { /* else using resume token */
                if (!content_length_found) {
                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "Could not create auto-ranged-put resume meta request; content_length must be specified.");
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    return NULL;
                }

                /* don't pass part size and total num parts. constructor will pick it up from token */
                return aws_s3_meta_request_auto_ranged_put_new(
                    client->allocator, client, 0, true, content_length, 0, options);
            }
        }
        case AWS_S3_META_REQUEST_TYPE_COPY_OBJECT: {
            return aws_s3_meta_request_copy_object_new(client->allocator, client, options);
        }
        case AWS_S3_META_REQUEST_TYPE_DEFAULT:
            if (options->operation_name.len == 0) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST, "Could not create Default Meta Request; operation name is required");
                aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                return NULL;
            }

            return aws_s3_meta_request_default_new(
                client->allocator,
                client,
                AWS_S3_REQUEST_TYPE_UNKNOWN,
                content_length,
                false /*should_compute_content_md5*/,
                options);
        default:
            AWS_FATAL_ASSERT(false);
    }

    return NULL;
}

static void s_s3_client_push_meta_request_synced(
    struct aws_s3_client *client,
    struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(meta_request);
    ASSERT_SYNCED_DATA_LOCK_HELD(client);

    struct aws_s3_meta_request_work *meta_request_work =
        aws_mem_calloc(client->allocator, 1, sizeof(struct aws_s3_meta_request_work));

    meta_request_work->meta_request = aws_s3_meta_request_acquire(meta_request);
    aws_linked_list_push_back(&client->synced_data.pending_meta_request_work, &meta_request_work->node);
}

static void s_s3_client_schedule_process_work_synced(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(client->vtable);
    AWS_PRECONDITION(client->vtable->schedule_process_work_synced);

    ASSERT_SYNCED_DATA_LOCK_HELD(client);

    client->vtable->schedule_process_work_synced(client);
}

static void s_s3_client_schedule_process_work_synced_default(struct aws_s3_client *client) {
    ASSERT_SYNCED_DATA_LOCK_HELD(client);

    if (client->synced_data.process_work_task_scheduled) {
        return;
    }

    aws_task_init(
        &client->synced_data.process_work_task, s_s3_client_process_work_task, client, "s3_client_process_work_task");

    aws_event_loop_schedule_task_now(client->process_work_event_loop, &client->synced_data.process_work_task);

    client->synced_data.process_work_task_scheduled = true;
}

/* Task function for trying to find a request that can be processed. */
static void s_s3_client_trim_buffer_pool_task(struct aws_task *task, void *arg, enum aws_task_status task_status) {
    AWS_PRECONDITION(task);
    (void)task;
    (void)task_status;

    if (task_status != AWS_TASK_STATUS_RUN_READY) {
        return;
    }

    struct aws_s3_client *client = arg;
    AWS_PRECONDITION(client);

    client->threaded_data.trim_buffer_pool_task_scheduled = false;

    uint32_t num_reqs_in_flight = (uint32_t)aws_atomic_load_int(&client->stats.num_requests_in_flight);

    if (num_reqs_in_flight == 0) {
        aws_s3_buffer_pool_trim(client->buffer_pool);
    }
}

static void s_s3_client_schedule_buffer_pool_trim_synced(struct aws_s3_client *client) {
    ASSERT_SYNCED_DATA_LOCK_HELD(client);

    if (client->threaded_data.trim_buffer_pool_task_scheduled) {
        return;
    }

    uint32_t num_reqs_in_flight = (uint32_t)aws_atomic_load_int(&client->stats.num_requests_in_flight);
    if (num_reqs_in_flight > 0) {
        return;
    }

    aws_task_init(
        &client->synced_data.trim_buffer_pool_task,
        s_s3_client_trim_buffer_pool_task,
        client,
        "s3_client_buffer_pool_trim_task");

    uint64_t trim_time = 0;
    aws_event_loop_current_clock_time(client->process_work_event_loop, &trim_time);
    trim_time +=
        aws_timestamp_convert(s_buffer_pool_trim_time_offset_in_s, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);

    aws_event_loop_schedule_task_future(
        client->process_work_event_loop, &client->synced_data.trim_buffer_pool_task, trim_time);

    client->threaded_data.trim_buffer_pool_task_scheduled = true;
}

static void s_s3_endpoints_cleanup_task(struct aws_task *task, void *arg, enum aws_task_status task_status) {
    (void)task;
    (void)task_status;

    struct aws_s3_client *client = arg;
    struct aws_array_list endpoints_to_release;
    aws_array_list_init_dynamic(&endpoints_to_release, client->allocator, 5, sizeof(struct aws_s3_endpoint *));

    /* BEGIN CRITICAL SECTION */
    aws_s3_client_lock_synced_data(client);
    client->synced_data.endpoints_cleanup_task_scheduled = false;

    for (struct aws_hash_iter iter = aws_hash_iter_begin(&client->synced_data.endpoints); !aws_hash_iter_done(&iter);
         aws_hash_iter_next(&iter)) {
        struct aws_s3_endpoint *endpoint = (struct aws_s3_endpoint *)iter.element.value;
        if (endpoint->client_synced_data.ref_count == 0) {
            aws_array_list_push_back(&endpoints_to_release, &endpoint);
            aws_hash_iter_delete(&iter, true);
        }
    }

    /* END CRITICAL SECTION */
    aws_s3_client_unlock_synced_data(client);

    /* now destroy all endpoints without holding the lock */
    size_t list_size = aws_array_list_length(&endpoints_to_release);
    for (size_t i = 0; i < list_size; ++i) {
        struct aws_s3_endpoint *endpoint;
        aws_array_list_get_at(&endpoints_to_release, &endpoint, i);
        aws_s3_endpoint_destroy(endpoint);
    }

    /* Clean up the array list */
    aws_array_list_clean_up(&endpoints_to_release);

    aws_s3_client_schedule_process_work(client);
}

static void s_s3_client_schedule_endpoints_cleanup_synced(struct aws_s3_client *client) {
    ASSERT_SYNCED_DATA_LOCK_HELD(client);
    if (client->synced_data.endpoints_cleanup_task_scheduled) {
        return;
    }
    client->synced_data.endpoints_cleanup_task_scheduled = true;
    uint64_t now_ns = 0;
    aws_event_loop_current_clock_time(client->process_work_event_loop, &now_ns);
    aws_event_loop_schedule_task_future(
        client->process_work_event_loop,
        &client->synced_data.endpoints_cleanup_task,
        now_ns +
            aws_timestamp_convert(s_endpoints_cleanup_time_offset_in_s, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL));
}

void aws_s3_client_schedule_process_work(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);
        s_s3_client_schedule_process_work_synced(client);
        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */
}

static void s_s3_client_remove_meta_request_threaded(
    struct aws_s3_client *client,
    struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(meta_request);
    (void)client;

    aws_linked_list_remove(&meta_request->client_process_work_threaded_data.node);
    meta_request->client_process_work_threaded_data.scheduled = false;
    aws_s3_meta_request_release(meta_request);
}

/* Task function for trying to find a request that can be processed. */
static void s_s3_client_process_work_task(struct aws_task *task, void *arg, enum aws_task_status task_status) {
    AWS_PRECONDITION(task);
    (void)task;
    (void)task_status;

    /* Client keeps a reference to the event loop group; a 'canceled' status should not happen.*/
    AWS_ASSERT(task_status == AWS_TASK_STATUS_RUN_READY);

    struct aws_s3_client *client = arg;
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(client->vtable);
    AWS_PRECONDITION(client->vtable->process_work);

    client->vtable->process_work(client);
}

static void s_s3_client_process_work_default(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(client->vtable);
    AWS_PRECONDITION(client->vtable->finish_destroy);

    struct aws_linked_list meta_request_work_list;
    aws_linked_list_init(&meta_request_work_list);

    /*******************/
    /* Step 1: Move relevant data into thread local memory and schedule cleanups */
    /*******************/
    AWS_LOGF_DEBUG(
        AWS_LS_S3_CLIENT,
        "id=%p s_s3_client_process_work_default - Moving relevant synced_data into threaded_data.",
        (void *)client);

    /* BEGIN CRITICAL SECTION */
    aws_s3_client_lock_synced_data(client);
    /* Once we exit this mutex, someone can reschedule this task. */
    client->synced_data.process_work_task_scheduled = false;
    client->synced_data.process_work_task_in_progress = true;

    if (client->synced_data.active) {
        s_s3_client_schedule_buffer_pool_trim_synced(client);
        s_s3_client_schedule_endpoints_cleanup_synced(client);
    } else if (client->synced_data.endpoints_cleanup_task_scheduled) {
        client->synced_data.endpoints_cleanup_task_scheduled = false;
        /* Cancel the task to run it sync */
        aws_s3_client_unlock_synced_data(client);
        aws_event_loop_cancel_task(client->process_work_event_loop, &client->synced_data.endpoints_cleanup_task);
        aws_s3_client_lock_synced_data(client);
    }

    aws_linked_list_swap_contents(&meta_request_work_list, &client->synced_data.pending_meta_request_work);

    uint32_t num_requests_queued =
        aws_s3_client_queue_requests_threaded(client, &client->synced_data.prepared_requests, false);

    {
        int sub_result = aws_sub_u32_checked(
            client->threaded_data.num_requests_being_prepared,
            num_requests_queued,
            &client->threaded_data.num_requests_being_prepared);

        AWS_ASSERT(sub_result == AWS_OP_SUCCESS);
        (void)sub_result;
    }

    {
        int sub_result = aws_sub_u32_checked(
            client->threaded_data.num_requests_being_prepared,
            client->synced_data.num_failed_prepare_requests,
            &client->threaded_data.num_requests_being_prepared);

        client->synced_data.num_failed_prepare_requests = 0;

        AWS_ASSERT(sub_result == AWS_OP_SUCCESS);
        (void)sub_result;
    }

    uint32_t num_endpoints_in_table = (uint32_t)aws_hash_table_get_entry_count(&client->synced_data.endpoints);
    uint32_t num_endpoints_allocated = client->synced_data.num_endpoints_allocated;

    aws_s3_client_unlock_synced_data(client);
    /* END CRITICAL SECTION */

    /*******************/
    /* Step 2: Push meta requests into the thread local list if they haven't already been scheduled. */
    /*******************/
    AWS_LOGF_DEBUG(
        AWS_LS_S3_CLIENT, "id=%p s_s3_client_process_work_default - Processing any new meta requests.", (void *)client);

    while (!aws_linked_list_empty(&meta_request_work_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_back(&meta_request_work_list);
        struct aws_s3_meta_request_work *meta_request_work =
            AWS_CONTAINER_OF(node, struct aws_s3_meta_request_work, node);

        AWS_FATAL_ASSERT(meta_request_work != NULL);
        AWS_FATAL_ASSERT(meta_request_work->meta_request != NULL);

        struct aws_s3_meta_request *meta_request = meta_request_work->meta_request;

        if (!meta_request->client_process_work_threaded_data.scheduled) {
            aws_linked_list_push_back(
                &client->threaded_data.meta_requests, &meta_request->client_process_work_threaded_data.node);

            meta_request->client_process_work_threaded_data.scheduled = true;
        } else {
            meta_request = aws_s3_meta_request_release(meta_request);
        }

        aws_mem_release(client->allocator, meta_request_work);
    }

    /*******************/
    /* Step 3: Update relevant meta requests and connections. */
    /*******************/
    {
        AWS_LOGF_DEBUG(AWS_LS_S3_CLIENT, "id=%p Updating meta requests.", (void *)client);
        aws_s3_client_update_meta_requests_threaded(client);

        AWS_LOGF_DEBUG(
            AWS_LS_S3_CLIENT, "id=%p Updating connections, assigning requests where possible.", (void *)client);
        aws_s3_client_update_connections_threaded(client);
    }

    /*******************/
    /* Step 4: Log client stats. */
    /*******************/
    {
        uint32_t num_requests_tracked_requests = (uint32_t)aws_atomic_load_int(&client->stats.num_requests_in_flight);

        uint32_t num_auto_ranged_get_network_io =
            s_s3_client_get_num_requests_network_io(client, AWS_S3_META_REQUEST_TYPE_GET_OBJECT);
        uint32_t num_auto_ranged_put_network_io =
            s_s3_client_get_num_requests_network_io(client, AWS_S3_META_REQUEST_TYPE_PUT_OBJECT);
        uint32_t num_auto_default_network_io =
            s_s3_client_get_num_requests_network_io(client, AWS_S3_META_REQUEST_TYPE_DEFAULT);

        uint32_t num_requests_network_io =
            s_s3_client_get_num_requests_network_io(client, AWS_S3_META_REQUEST_TYPE_MAX);

        uint32_t num_requests_stream_queued_waiting =
            (uint32_t)aws_atomic_load_int(&client->stats.num_requests_stream_queued_waiting);

        uint32_t num_requests_being_prepared = client->threaded_data.num_requests_being_prepared;

        uint32_t num_requests_streaming_response =
            (uint32_t)aws_atomic_load_int(&client->stats.num_requests_streaming_response);

        uint32_t total_approx_requests = num_requests_network_io + num_requests_stream_queued_waiting +
                                         num_requests_streaming_response + num_requests_being_prepared +
                                         client->threaded_data.request_queue_size;
        AWS_LOGF(
            s_log_level_client_stats,
            AWS_LS_S3_CLIENT_STATS,
            "id=%p Requests-in-flight(approx/exact):%d/%d  Requests-preparing:%d  Requests-queued:%d  "
            "Requests-network(get/put/default/total):%d/%d/%d/%d  Requests-streaming-waiting:%d  "
            "Requests-streaming-response:%d "
            " Endpoints(in-table/allocated):%d/%d",
            (void *)client,
            total_approx_requests,
            num_requests_tracked_requests,
            num_requests_being_prepared,
            client->threaded_data.request_queue_size,
            num_auto_ranged_get_network_io,
            num_auto_ranged_put_network_io,
            num_auto_default_network_io,
            num_requests_network_io,
            num_requests_stream_queued_waiting,
            num_requests_streaming_response,
            num_endpoints_in_table,
            num_endpoints_allocated);
    }

    /*******************/
    /* Step 5: Check for client shutdown. */
    /*******************/
    {
        /* BEGIN CRITICAL SECTION */
        aws_s3_client_lock_synced_data(client);
        client->synced_data.process_work_task_in_progress = false;

        /* This flag should never be set twice. If it was, that means a double-free could occur.*/
        AWS_ASSERT(!client->synced_data.finish_destroy);

        bool finish_destroy =
            client->synced_data.active == false && client->synced_data.start_destroy_executing == false &&
            client->synced_data.body_streaming_elg_allocated == false &&
            client->synced_data.process_work_task_scheduled == false &&
            client->synced_data.process_work_task_in_progress == false &&
            client->synced_data.s3express_provider_active == false && client->synced_data.num_endpoints_allocated == 0;

        client->synced_data.finish_destroy = finish_destroy;

        if (!client->synced_data.active) {
            AWS_LOGF_DEBUG(
                AWS_LS_S3_CLIENT,
                "id=%p Client shutdown progress: starting_destroy_executing=%d  body_streaming_elg_allocated=%d  "
                "process_work_task_scheduled=%d  process_work_task_in_progress=%d  num_endpoints_allocated=%d "
                "s3express_provider_active=%d finish_destroy=%d",
                (void *)client,
                (int)client->synced_data.start_destroy_executing,
                (int)client->synced_data.body_streaming_elg_allocated,
                (int)client->synced_data.process_work_task_scheduled,
                (int)client->synced_data.process_work_task_in_progress,
                (int)client->synced_data.num_endpoints_allocated,
                (int)client->synced_data.s3express_provider_active,
                (int)client->synced_data.finish_destroy);
        }

        aws_s3_client_unlock_synced_data(client);
        /* END CRITICAL SECTION */

        if (finish_destroy) {
            client->vtable->finish_destroy(client);
        }
    }
}

static void s_s3_client_prepare_callback_queue_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code,
    void *user_data);

static bool s_s3_client_should_update_meta_request(
    struct aws_s3_client *client,
    struct aws_s3_meta_request *meta_request,
    uint32_t num_requests_in_flight,
    const uint32_t max_requests_in_flight,
    const uint32_t max_requests_prepare) {

    /* CreateSession has high priority to bypass the checks. */
    if (meta_request->type == AWS_S3_META_REQUEST_TYPE_DEFAULT) {
        struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
        if (meta_request_default->request_type == AWS_S3_REQUEST_TYPE_CREATE_SESSION) {
            return true;
        }
    }

    /**
     * If number of being-prepared + already-prepared-and-queued requests is more than the max that can
     * be in the preparation stage.
     * Or total number of requests tracked by the client is more than the max tracked ("in flight")
     * requests.
     *
     * We cannot create more requests for this meta request.
     */
    if ((client->threaded_data.num_requests_being_prepared + client->threaded_data.request_queue_size) >=
        max_requests_prepare) {
        return false;
    }
    if (num_requests_in_flight >= max_requests_in_flight) {
        return false;
    }

    /* If this particular endpoint doesn't have any known addresses yet, then we don't want to go full speed in
     * ramping up requests just yet. If there is already enough in the queue for one address (even if those
     * aren't for this particular endpoint) we skip over this meta request for now. */
    struct aws_s3_endpoint *endpoint = meta_request->endpoint;
    AWS_ASSERT(endpoint != NULL);
    AWS_ASSERT(client->vtable->get_host_address_count);
    size_t num_known_vips = client->vtable->get_host_address_count(
        client->client_bootstrap->host_resolver, endpoint->host_name, AWS_GET_HOST_ADDRESS_COUNT_RECORD_TYPE_A);
    if (num_known_vips == 0 && (client->threaded_data.num_requests_being_prepared +
                                client->threaded_data.request_queue_size) >= g_min_num_connections) {
        return false;
    }

    /* Nothing blocks the meta request to create more requests */
    return true;
}

void aws_s3_client_update_meta_requests_threaded(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    const uint32_t max_requests_in_flight = aws_s3_client_get_max_requests_in_flight(client);
    const uint32_t max_requests_prepare = aws_s3_client_get_max_requests_prepare(client);

    struct aws_linked_list meta_requests_work_remaining;
    aws_linked_list_init(&meta_requests_work_remaining);

    uint32_t num_requests_in_flight = (uint32_t)aws_atomic_load_int(&client->stats.num_requests_in_flight);

    const uint32_t pass_flags[] = {
        AWS_S3_META_REQUEST_UPDATE_FLAG_CONSERVATIVE,
        0,
    };

    const uint32_t num_passes = AWS_ARRAY_SIZE(pass_flags);

    aws_s3_buffer_pool_remove_reservation_hold(client->buffer_pool);

    for (uint32_t pass_index = 0; pass_index < num_passes; ++pass_index) {

        /**
         * Iterate through the meta requests to update meta requests and get new requests that can then be prepared
+         * (reading from any streams, signing, etc.) for sending.
         */
        while (!aws_linked_list_empty(&client->threaded_data.meta_requests)) {

            struct aws_linked_list_node *meta_request_node =
                aws_linked_list_begin(&client->threaded_data.meta_requests);
            struct aws_s3_meta_request *meta_request =
                AWS_CONTAINER_OF(meta_request_node, struct aws_s3_meta_request, client_process_work_threaded_data);

            if (!s_s3_client_should_update_meta_request(
                    client, meta_request, num_requests_in_flight, max_requests_in_flight, max_requests_prepare)) {

                /* Move the meta request to be processed from next loop. */
                aws_linked_list_remove(&meta_request->client_process_work_threaded_data.node);
                aws_linked_list_push_back(
                    &meta_requests_work_remaining, &meta_request->client_process_work_threaded_data.node);
                continue;
            }

            struct aws_s3_request *request = NULL;

            /* Try to grab the next request from the meta request. */
            /* TODO: should we bail out if request fails to update due to mem or
             * continue going and hopping that following reqs can fit into mem?
             * check if avail space is at least part size? */
            bool work_remaining = aws_s3_meta_request_update(meta_request, pass_flags[pass_index], &request);

            if (work_remaining) {
                /* If there is work remaining, but we didn't get a request back, take the meta request out of the
                 * list so that we don't use it again during this function, with the intention of putting it back in
                 * the list before this function ends. */
                if (request == NULL) {
                    aws_linked_list_remove(&meta_request->client_process_work_threaded_data.node);
                    aws_linked_list_push_back(
                        &meta_requests_work_remaining, &meta_request->client_process_work_threaded_data.node);
                } else {
                    request->tracked_by_client = true;

                    ++client->threaded_data.num_requests_being_prepared;

                    num_requests_in_flight =
                        (uint32_t)aws_atomic_fetch_add(&client->stats.num_requests_in_flight, 1) + 1;

                    aws_s3_meta_request_prepare_request(
                        meta_request, request, s_s3_client_prepare_callback_queue_request, client);
                }
            } else {
                s_s3_client_remove_meta_request_threaded(client, meta_request);
            }
        }

        aws_linked_list_move_all_front(&client->threaded_data.meta_requests, &meta_requests_work_remaining);
    }
}

static void s_s3_client_meta_request_finished_request(
    struct aws_s3_client *client,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(request);

    if (request->tracked_by_client) {
        /* BEGIN CRITICAL SECTION */
        aws_s3_client_lock_synced_data(client);
        aws_atomic_fetch_sub(&client->stats.num_requests_in_flight, 1);
        s_s3_client_schedule_process_work_synced(client);
        aws_s3_client_unlock_synced_data(client);
        /* END CRITICAL SECTION */
    }
    aws_s3_meta_request_finished_request(meta_request, request, error_code);
}

static void s_s3_client_prepare_callback_queue_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code,
    void *user_data) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);

    struct aws_s3_client *client = user_data;
    AWS_PRECONDITION(client);

    if (error_code != AWS_ERROR_SUCCESS) {
        s_s3_client_meta_request_finished_request(client, meta_request, request, error_code);
        request = aws_s3_request_release(request);
    }

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);

        if (error_code == AWS_ERROR_SUCCESS) {
            aws_linked_list_push_back(&client->synced_data.prepared_requests, &request->node);
        } else {
            ++client->synced_data.num_failed_prepare_requests;
        }

        s_s3_client_schedule_process_work_synced(client);
        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */
}

void aws_s3_client_update_connections_threaded(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(client->vtable);

    struct aws_linked_list left_over_requests;
    aws_linked_list_init(&left_over_requests);

    while (s_s3_client_get_num_requests_network_io(client, AWS_S3_META_REQUEST_TYPE_MAX) <
               aws_s3_client_get_max_active_connections(client, NULL) &&
           !aws_linked_list_empty(&client->threaded_data.request_queue)) {

        struct aws_s3_request *request = aws_s3_client_dequeue_request_threaded(client);
        const uint32_t max_active_connections = aws_s3_client_get_max_active_connections(client, request->meta_request);
        if (request->is_noop) {
            /* If request is no-op, finishes and cleans up the request */
            s_s3_client_meta_request_finished_request(client, request->meta_request, request, AWS_ERROR_SUCCESS);
            request = aws_s3_request_release(request);
        } else if (!request->always_send && aws_s3_meta_request_has_finish_result(request->meta_request)) {
            /* Unless the request is marked "always send", if this meta request has a finish result, then finish the
             * request now and release it. */
            s_s3_client_meta_request_finished_request(client, request->meta_request, request, AWS_ERROR_S3_CANCELED);
            request = aws_s3_request_release(request);
        } else if (
            s_s3_client_get_num_requests_network_io(client, request->meta_request->type) < max_active_connections) {
            s_s3_client_create_connection_for_request(client, request);
        } else {
            /* Push the request into the left-over list to be used in a future call of this function. */
            aws_linked_list_push_back(&left_over_requests, &request->node);
        }
    }

    aws_s3_client_queue_requests_threaded(client, &left_over_requests, true);
}

static void s_s3_client_acquired_retry_token(
    struct aws_retry_strategy *retry_strategy,
    int error_code,
    struct aws_retry_token *token,
    void *user_data);

static void s_s3_client_retry_ready(struct aws_retry_token *token, int error_code, void *user_data);

static void s_s3_client_create_connection_for_request_default(
    struct aws_s3_client *client,
    struct aws_s3_request *request);

static void s_s3_client_create_connection_for_request(struct aws_s3_client *client, struct aws_s3_request *request) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(client->vtable);

    if (client->vtable->create_connection_for_request) {
        client->vtable->create_connection_for_request(client, request);
        return;
    }

    s_s3_client_create_connection_for_request_default(client, request);
}

static void s_s3_client_create_connection_for_request_default(
    struct aws_s3_client *client,
    struct aws_s3_request *request) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request *meta_request = request->meta_request;
    AWS_PRECONDITION(meta_request);

    aws_atomic_fetch_add(&client->stats.num_requests_network_io[meta_request->type], 1);

    struct aws_s3_connection *connection = aws_mem_calloc(client->allocator, 1, sizeof(struct aws_s3_connection));

    connection->endpoint = aws_s3_endpoint_acquire(meta_request->endpoint, false /*already_holding_lock*/);
    connection->request = request;

    struct aws_byte_cursor host_header_value;
    AWS_ZERO_STRUCT(host_header_value);

    struct aws_http_headers *message_headers = aws_http_message_get_headers(meta_request->initial_request_message);
    AWS_ASSERT(message_headers);

    int result = aws_http_headers_get(message_headers, g_host_header_name, &host_header_value);
    AWS_ASSERT(result == AWS_OP_SUCCESS);
    (void)result;

    if (aws_retry_strategy_acquire_retry_token(
            client->retry_strategy, &host_header_value, s_s3_client_acquired_retry_token, connection, 0)) {

        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Client could not acquire retry token for request %p due to error %d (%s)",
            (void *)client,
            (void *)request,
            aws_last_error_or_unknown(),
            aws_error_str(aws_last_error_or_unknown()));

        goto reset_connection;
    }

    return;

reset_connection:

    aws_s3_client_notify_connection_finished(
        client, connection, aws_last_error_or_unknown(), AWS_S3_CONNECTION_FINISH_CODE_FAILED);
}

static void s_s3_client_acquired_retry_token(
    struct aws_retry_strategy *retry_strategy,
    int error_code,
    struct aws_retry_token *token,
    void *user_data) {

    AWS_PRECONDITION(retry_strategy);
    (void)retry_strategy;

    struct aws_s3_connection *connection = user_data;
    AWS_PRECONDITION(connection);

    struct aws_s3_request *request = connection->request;
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request *meta_request = request->meta_request;
    AWS_PRECONDITION(meta_request);

    struct aws_s3_endpoint *endpoint = meta_request->endpoint;
    AWS_ASSERT(endpoint != NULL);

    struct aws_s3_client *client = endpoint->client;
    AWS_ASSERT(client != NULL);

    if (error_code != AWS_ERROR_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Client could not get retry token for connection %p processing request %p due to error %d (%s)",
            (void *)client,
            (void *)connection,
            (void *)request,
            error_code,
            aws_error_str(error_code));

        goto error_clean_up;
    }

    AWS_ASSERT(token);

    connection->retry_token = token;

    AWS_ASSERT(client->vtable->acquire_http_connection);

    /* client needs to be kept alive until s_s3_client_on_acquire_http_connection completes */
    /* TODO: not a blocker, consider managing the life time of aws_s3_client from aws_s3_endpoint to simplify usage */
    aws_s3_client_acquire(client);

    client->vtable->acquire_http_connection(
        endpoint->http_connection_manager, s_s3_client_on_acquire_http_connection, connection);

    return;

error_clean_up:

    aws_s3_client_notify_connection_finished(client, connection, error_code, AWS_S3_CONNECTION_FINISH_CODE_FAILED);
}

static void s_s3_client_on_acquire_http_connection(
    struct aws_http_connection *incoming_http_connection,
    int error_code,
    void *user_data) {

    struct aws_s3_connection *connection = user_data;
    AWS_PRECONDITION(connection);

    struct aws_s3_request *request = connection->request;
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request *meta_request = request->meta_request;
    AWS_PRECONDITION(meta_request);

    struct aws_s3_endpoint *endpoint = meta_request->endpoint;
    AWS_ASSERT(endpoint != NULL);

    struct aws_s3_client *client = endpoint->client;
    AWS_ASSERT(client != NULL);

    if (error_code != AWS_ERROR_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_ENDPOINT,
            "id=%p: Could not acquire connection due to error code %d (%s)",
            (void *)endpoint,
            error_code,
            aws_error_str(error_code));

        if (error_code == AWS_IO_DNS_INVALID_NAME || error_code == AWS_IO_TLS_ERROR_NEGOTIATION_FAILURE ||
            error_code == AWS_ERROR_PLATFORM_NOT_SUPPORTED || error_code == AWS_IO_SOCKET_INVALID_OPTIONS) {
            /**
             * Fall fast without retry
             * - Invalid DNS name will not change after retry.
             * - TLS negotiation is expensive and retry will not help in most case.
             */
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "id=%p Meta request cannot recover from error %d (%s) while acquiring HTTP connection. (request=%p)",
                (void *)meta_request,
                error_code,
                aws_error_str(error_code),
                (void *)request);
            goto error_fail;
        }

        goto error_retry;
    }

    connection->http_connection = incoming_http_connection;
    aws_s3_meta_request_send_request(meta_request, connection);
    aws_s3_client_release(client); /* kept since this callback was registered */
    return;

error_retry:

    aws_s3_client_notify_connection_finished(client, connection, error_code, AWS_S3_CONNECTION_FINISH_CODE_RETRY);
    aws_s3_client_release(client); /* kept since this callback was registered */
    return;

error_fail:

    aws_s3_client_notify_connection_finished(client, connection, error_code, AWS_S3_CONNECTION_FINISH_CODE_FAILED);
    aws_s3_client_release(client); /* kept since this callback was registered */
}

/* Called by aws_s3_meta_request when it has finished using this connection for a single request. */
void aws_s3_client_notify_connection_finished(
    struct aws_s3_client *client,
    struct aws_s3_connection *connection,
    int error_code,
    enum aws_s3_connection_finish_code finish_code) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(connection);

    struct aws_s3_request *request = connection->request;
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request *meta_request = request->meta_request;

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->initial_request_message);

    struct aws_s3_endpoint *endpoint = meta_request->endpoint;
    AWS_PRECONDITION(endpoint);
    if (request->send_data.metrics) {
        request->send_data.metrics->crt_info_metrics.error_code = error_code;
    }

    /* If we're trying to set up a retry... */
    if (finish_code == AWS_S3_CONNECTION_FINISH_CODE_RETRY) {

        if (connection->retry_token == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_CLIENT,
                "id=%p Client could not schedule retry of request %p for meta request %p, as retry token is NULL.",
                (void *)client,
                (void *)request,
                (void *)meta_request);

            goto reset_connection;
        }

        if (aws_s3_meta_request_is_finished(meta_request)) {
            AWS_LOGF_DEBUG(
                AWS_LS_S3_CLIENT,
                "id=%p Client not scheduling retry of request %p for meta request %p with token %p because meta "
                "request has been flagged as finished.",
                (void *)client,
                (void *)request,
                (void *)meta_request,
                (void *)connection->retry_token);

            goto reset_connection;
        }

        AWS_LOGF_DEBUG(
            AWS_LS_S3_CLIENT,
            "id=%p Client scheduling retry of request %p for meta request %p with token %p with error code %d (%s).",
            (void *)client,
            (void *)request,
            (void *)meta_request,
            (void *)connection->retry_token,
            error_code,
            aws_error_str(error_code));

        enum aws_retry_error_type error_type = AWS_RETRY_ERROR_TYPE_TRANSIENT;

        switch (error_code) {
            case AWS_ERROR_S3_INTERNAL_ERROR:
                error_type = AWS_RETRY_ERROR_TYPE_SERVER_ERROR;
                break;

            case AWS_ERROR_S3_SLOW_DOWN:
                error_type = AWS_RETRY_ERROR_TYPE_THROTTLING;
                break;
        }

        if (connection->http_connection != NULL) {
            AWS_ASSERT(endpoint->http_connection_manager);

            aws_http_connection_manager_release_connection(
                endpoint->http_connection_manager, connection->http_connection);

            connection->http_connection = NULL;
        }

        /* Ask the retry strategy to schedule a retry of the request. */
        if (aws_retry_strategy_schedule_retry(
                connection->retry_token, error_type, s_s3_client_retry_ready, connection)) {

            AWS_LOGF_ERROR(
                AWS_LS_S3_CLIENT,
                "id=%p Client could not retry request %p for meta request %p with token %p due to error %d (%s)",
                (void *)client,
                (void *)request,
                (void *)meta_request,
                (void *)connection->retry_token,
                aws_last_error_or_unknown(),
                aws_error_str(aws_last_error_or_unknown()));

            goto reset_connection;
        }

        return;
    }

reset_connection:

    if (connection->retry_token != NULL) {
        /* If we have a retry token and successfully finished, record that success. */
        if (finish_code == AWS_S3_CONNECTION_FINISH_CODE_SUCCESS) {
            aws_retry_token_record_success(connection->retry_token);
        }

        aws_retry_token_release(connection->retry_token);
        connection->retry_token = NULL;
    }

    /* If we weren't successful, and we're here, that means this failure is not eligible for a retry. So finish the
     * request, and close our HTTP connection. */
    if (finish_code != AWS_S3_CONNECTION_FINISH_CODE_SUCCESS) {
        if (connection->http_connection != NULL) {
            aws_http_connection_close(connection->http_connection);
        }
    }

    aws_atomic_fetch_sub(&client->stats.num_requests_network_io[meta_request->type], 1);

    s_s3_client_meta_request_finished_request(client, meta_request, request, error_code);

    if (connection->http_connection != NULL) {
        AWS_ASSERT(endpoint->http_connection_manager);

        aws_http_connection_manager_release_connection(endpoint->http_connection_manager, connection->http_connection);

        connection->http_connection = NULL;
    }

    if (connection->request != NULL) {
        connection->request = aws_s3_request_release(connection->request);
    }

    aws_retry_token_release(connection->retry_token);
    connection->retry_token = NULL;

    aws_s3_endpoint_release(connection->endpoint);
    connection->endpoint = NULL;

    aws_mem_release(client->allocator, connection);
    connection = NULL;

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_client_lock_synced_data(client);
        s_s3_client_schedule_process_work_synced(client);
        aws_s3_client_unlock_synced_data(client);
    }
    /* END CRITICAL SECTION */
}

static void s_s3_client_prepare_request_callback_retry_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code,
    void *user_data);

static void s_s3_client_retry_ready(struct aws_retry_token *token, int error_code, void *user_data) {
    AWS_PRECONDITION(token);
    (void)token;

    struct aws_s3_connection *connection = user_data;
    AWS_PRECONDITION(connection);

    struct aws_s3_request *request = connection->request;
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request *meta_request = request->meta_request;
    AWS_PRECONDITION(meta_request);

    struct aws_s3_endpoint *endpoint = meta_request->endpoint;
    AWS_PRECONDITION(endpoint);

    struct aws_s3_client *client = endpoint->client;
    AWS_PRECONDITION(client);

    /* If we couldn't retry this request, then bail on the entire meta request. */
    if (error_code != AWS_ERROR_SUCCESS) {

        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p Client could not retry request %p for meta request %p due to error %d (%s)",
            (void *)client,
            (void *)meta_request,
            (void *)request,
            error_code,
            aws_error_str(error_code));

        goto error_clean_up;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p Client retrying request %p for meta request %p on connection %p with retry token %p",
        (void *)client,
        (void *)request,
        (void *)meta_request,
        (void *)connection,
        (void *)connection->retry_token);

    aws_s3_meta_request_prepare_request(
        meta_request, request, s_s3_client_prepare_request_callback_retry_request, connection);

    return;

error_clean_up:

    aws_s3_client_notify_connection_finished(client, connection, error_code, AWS_S3_CONNECTION_FINISH_CODE_FAILED);
}

static void s_s3_client_prepare_request_callback_retry_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code,
    void *user_data) {
    AWS_PRECONDITION(meta_request);
    (void)meta_request;

    AWS_PRECONDITION(request);
    (void)request;

    struct aws_s3_connection *connection = user_data;
    AWS_PRECONDITION(connection);

    struct aws_s3_endpoint *endpoint = meta_request->endpoint;
    AWS_ASSERT(endpoint != NULL);

    struct aws_s3_client *client = endpoint->client;
    AWS_ASSERT(client != NULL);

    if (error_code == AWS_ERROR_SUCCESS) {
        AWS_ASSERT(connection->retry_token);

        s_s3_client_acquired_retry_token(
            client->retry_strategy, AWS_ERROR_SUCCESS, connection->retry_token, connection);
    } else {
        aws_s3_client_notify_connection_finished(client, connection, error_code, AWS_S3_CONNECTION_FINISH_CODE_FAILED);
    }
}

static void s_resume_token_ref_count_zero_callback(void *arg) {
    struct aws_s3_meta_request_resume_token *token = arg;

    aws_string_destroy(token->multipart_upload_id);

    aws_mem_release(token->allocator, token);
}

struct aws_s3_meta_request_resume_token *aws_s3_meta_request_resume_token_new(struct aws_allocator *allocator) {
    struct aws_s3_meta_request_resume_token *token =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_meta_request_resume_token));

    token->allocator = allocator;
    aws_ref_count_init(&token->ref_count, token, s_resume_token_ref_count_zero_callback);

    return token;
}

struct aws_s3_meta_request_resume_token *aws_s3_meta_request_resume_token_new_upload(
    struct aws_allocator *allocator,
    const struct aws_s3_upload_resume_token_options *options) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(options);
    if (options->part_size > SIZE_MAX) {
        aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
        return NULL;
    }

    struct aws_s3_meta_request_resume_token *token = aws_s3_meta_request_resume_token_new(allocator);
    token->multipart_upload_id = aws_string_new_from_cursor(allocator, &options->upload_id);
    token->part_size = (size_t)options->part_size;
    token->total_num_parts = options->total_num_parts;
    token->num_parts_completed = options->num_parts_completed;
    token->type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT;
    return token;
}

struct aws_s3_meta_request_resume_token *aws_s3_meta_request_resume_token_acquire(
    struct aws_s3_meta_request_resume_token *resume_token) {
    if (resume_token) {
        aws_ref_count_acquire(&resume_token->ref_count);
    }
    return resume_token;
}

struct aws_s3_meta_request_resume_token *aws_s3_meta_request_resume_token_release(
    struct aws_s3_meta_request_resume_token *resume_token) {
    if (resume_token) {
        aws_ref_count_release(&resume_token->ref_count);
    }
    return NULL;
}

enum aws_s3_meta_request_type aws_s3_meta_request_resume_token_type(
    struct aws_s3_meta_request_resume_token *resume_token) {
    AWS_FATAL_PRECONDITION(resume_token);
    return resume_token->type;
}

uint64_t aws_s3_meta_request_resume_token_part_size(struct aws_s3_meta_request_resume_token *resume_token) {
    AWS_FATAL_PRECONDITION(resume_token);
    return (uint64_t)resume_token->part_size;
}

size_t aws_s3_meta_request_resume_token_total_num_parts(struct aws_s3_meta_request_resume_token *resume_token) {
    AWS_FATAL_PRECONDITION(resume_token);
    return resume_token->total_num_parts;
}

size_t aws_s3_meta_request_resume_token_num_parts_completed(struct aws_s3_meta_request_resume_token *resume_token) {
    AWS_FATAL_PRECONDITION(resume_token);
    return resume_token->num_parts_completed;
}

struct aws_byte_cursor aws_s3_meta_request_resume_token_upload_id(
    struct aws_s3_meta_request_resume_token *resume_token) {
    AWS_FATAL_PRECONDITION(resume_token);
    if (resume_token->type == AWS_S3_META_REQUEST_TYPE_PUT_OBJECT && resume_token->multipart_upload_id != NULL) {
        return aws_byte_cursor_from_string(resume_token->multipart_upload_id);
    }

    return aws_byte_cursor_from_c_str("");
}

static uint64_t s_upload_timeout_threshold_ns = 5000000000; /* 5 Secs */
const size_t g_expect_timeout_offset_ms =
    700; /* 0.7 Secs. From experiments on c5n.18xlarge machine for 30 GiB upload, it gave us best performance. */

/**
 * The upload timeout optimization: explained.
 *
 * Sometimes, S3 is extremely slow responding to an upload.
 * In these cases, it's much faster to cancel and resend the upload,
 * vs waiting 5sec for the slow response.
 *
 * Typically, S3 responds to an upload in 0.2sec after the request is fully received.
 * But occasionally (about 0.1%) it takes 5sec to respond.
 * In a large 30GiB file upload, you can expect about 4 parts to suffer from
 * a slow response. If one of these parts is near the end of the file,
 * then we end up sitting around doing nothing for up to 5sec, waiting
 * for this final slow upload to complete.
 *
 * We use the response_first_byte_timeout HTTP option to cancel uploads
 * suffering from a slow response. But how should we set it? A fast 100Gbps
 * machine definitely wants it! But a slow computer does not. A slow computer
 * would be better off waiting 5sec for the response, vs re-uploading the whole request.
 *
 * The current algorithm:
 * 1. Start without a timeout value. After 10 requests completed, we know the average of how long the
 *      request takes. We decide if it's worth to set a timeout value or not. (If the average of request takes more than
 *      5 secs or not) TODO: if the client have different part size, this doesn't make sense
 * 2. If it is worth to retry, start with a default timeout value, 1 sec.
 * 3. If a request finishes successfully, use the average response_to_first_byte_time + g_expect_timeout_offset_ms as
 *      our expected timeout value. (TODO: The real expected timeout value should be a P99 of all the requests.)
 *  3.1 Adjust the current timeout value against the expected timeout value, via 0.99 * <current timeout> + 0.01 *
 *      <expected timeout> to get closer to the expected timeout value.
 * 4. If request had timed out. We check the timeout rate.
 *  4.1 If timeout rate is larger than 0.1%, we increase the timeout value by 100ms (Check the timeout value when the
 *      request was made, if the updated timeout value is larger than the expected, skip update).
 *  4.2 If timeout rate is larger than 1%, we increase the timeout value by 1 secs (If needed). And clear the rate
 *      to get the exact rate with new timeout value.
 *  4.3 Once the timeout value is larger than 5 secs, we stop the process.
 *
 * Invoked from `s_s3_auto_ranged_put_send_request_finish`.
 */
void aws_s3_client_update_upload_part_timeout(
    struct aws_s3_client *client,
    struct aws_s3_request *finished_upload_part_request,
    int finished_error_code) {

    aws_s3_client_lock_synced_data(client);
    struct aws_s3_upload_part_timeout_stats *stats = &client->synced_data.upload_part_stats;
    if (stats->stop_timeout) {
        /* Timeout was disabled */
        goto unlock;
    }

    struct aws_s3_request_metrics *metrics = finished_upload_part_request->send_data.metrics;
    size_t current_timeout_ms = aws_atomic_load_int(&client->upload_timeout_ms);
    uint64_t current_timeout_ns =
        aws_timestamp_convert(current_timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
    uint64_t updated_timeout_ns = 0;
    uint64_t expect_timeout_offset_ns =
        aws_timestamp_convert(g_expect_timeout_offset_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);

    switch (finished_error_code) {
        case AWS_ERROR_SUCCESS:
            /* We only interested in request succeed */
            stats->num_successful_upload_requests = aws_add_u64_saturating(stats->num_successful_upload_requests, 1);
            if (stats->num_successful_upload_requests <= 10) {
                /* Gether the data */
                uint64_t request_time_ns =
                    metrics->time_metrics.receive_end_timestamp_ns - metrics->time_metrics.send_start_timestamp_ns;
                stats->initial_request_time.sum_ns =
                    aws_add_u64_saturating(stats->initial_request_time.sum_ns, request_time_ns);
                ++stats->initial_request_time.num_samples;
                if (stats->num_successful_upload_requests == 10) {
                    /* Decide we need a timeout or not */
                    uint64_t average_request_time_ns =
                        stats->initial_request_time.sum_ns / stats->initial_request_time.num_samples;
                    if (average_request_time_ns >= s_upload_timeout_threshold_ns) {
                        /* We don't need a timeout, as retry will be slower than just wait for the server to response */
                        stats->stop_timeout = true;
                    } else {
                        /* Start the timeout at 1 secs */
                        aws_atomic_store_int(&client->upload_timeout_ms, 1000);
                    }
                }
                goto unlock;
            }
            /* Starts to update timeout on case of succeed */
            stats->timeout_rate_tracking.num_completed =
                aws_add_u64_saturating(stats->timeout_rate_tracking.num_completed, 1);
            /* Response to first byte is time taken for the first byte data received from the request finished
             * sending */
            uint64_t response_to_first_byte_time_ns =
                metrics->time_metrics.receive_start_timestamp_ns - metrics->time_metrics.send_end_timestamp_ns;
            stats->response_to_first_byte_time.sum_ns =
                aws_add_u64_saturating(stats->response_to_first_byte_time.sum_ns, response_to_first_byte_time_ns);
            stats->response_to_first_byte_time.num_samples =
                aws_add_u64_saturating(stats->response_to_first_byte_time.num_samples, 1);

            uint64_t average_response_to_first_byte_time_ns =
                stats->response_to_first_byte_time.sum_ns / stats->response_to_first_byte_time.num_samples;
            uint64_t expected_timeout_ns = average_response_to_first_byte_time_ns + expect_timeout_offset_ns;
            double timeout_ns_double = (double)current_timeout_ns * 0.99 + (double)expected_timeout_ns * 0.01;
            updated_timeout_ns = (uint64_t)timeout_ns_double;
            break;

        case AWS_ERROR_HTTP_RESPONSE_FIRST_BYTE_TIMEOUT:
            if (stats->num_successful_upload_requests < 10) {
                goto unlock;
            }

            /* Starts to update timeout on case of timed out */
            stats->timeout_rate_tracking.num_completed =
                aws_add_u64_saturating(stats->timeout_rate_tracking.num_completed, 1);
            stats->timeout_rate_tracking.num_failed =
                aws_add_u64_saturating(stats->timeout_rate_tracking.num_failed, 1);

            uint64_t timeout_threshold = (uint64_t)ceil((double)stats->timeout_rate_tracking.num_completed / 100);
            uint64_t warning_threshold = (uint64_t)ceil((double)stats->timeout_rate_tracking.num_completed / 1000);

            if (stats->timeout_rate_tracking.num_failed > timeout_threshold) {
                /**
                 * Restore the rate track, as we are larger than 1%, it goes off the record.
                 */

                AWS_LOGF_WARN(
                    AWS_LS_S3_CLIENT,
                    "id=%p Client upload part timeout rate is larger than expected, current timeout is %zu, bump it "
                    "up. Request original timeout is: %zu",
                    (void *)client,
                    current_timeout_ms,
                    finished_upload_part_request->upload_timeout_ms);
                stats->timeout_rate_tracking.num_completed = 0;
                stats->timeout_rate_tracking.num_failed = 0;
                if (finished_upload_part_request->upload_timeout_ms + 1000 > current_timeout_ms) {
                    /* Update the timeout by adding 1 secs only when it's worth to do so */
                    updated_timeout_ns = aws_add_u64_saturating(
                        current_timeout_ns, aws_timestamp_convert(1, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL));
                }
            } else if (stats->timeout_rate_tracking.num_failed > warning_threshold) {
                if (finished_upload_part_request->upload_timeout_ms + 100 > current_timeout_ms) {
                    /* Only update the timeout by adding 100 ms if the request was made with a longer time out. */
                    updated_timeout_ns = aws_add_u64_saturating(
                        current_timeout_ns,
                        aws_timestamp_convert(100, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL));
                }
            }
            break;
        default:
            break;
    }

    if (updated_timeout_ns != 0) {
        if (updated_timeout_ns > s_upload_timeout_threshold_ns) {
            /* Stops timeout, as wait for server to response will be faster to set our own timeout */
            stats->stop_timeout = true;
            /* Unset the upload_timeout */
            updated_timeout_ns = 0;
        }
        /* Apply the updated timeout */
        aws_atomic_store_int(
            &client->upload_timeout_ms,
            (size_t)aws_timestamp_convert(updated_timeout_ns, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_MILLIS, NULL));
    }

unlock:
    aws_s3_client_unlock_synced_data(client);
}
