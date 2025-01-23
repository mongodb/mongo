/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_request.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_util.h"
#include <aws/auth/signable.h>
#include <aws/common/clock.h>
#include <aws/io/stream.h>
#include <aws/s3/s3_client.h>

static void s_s3_request_destroy(void *user_data);

struct aws_s3_request *aws_s3_request_new(
    struct aws_s3_meta_request *meta_request,
    int request_tag,
    enum aws_s3_request_type request_type,
    uint32_t part_number,
    uint32_t flags) {

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->allocator);

    struct aws_s3_request *request = aws_mem_calloc(meta_request->allocator, 1, sizeof(struct aws_s3_request));

    aws_ref_count_init(&request->ref_count, request, (aws_simple_completion_callback *)s_s3_request_destroy);

    request->allocator = meta_request->allocator;
    request->meta_request = aws_s3_meta_request_acquire(meta_request);

    request->request_tag = request_tag;
    request->request_type = request_type;

    request->operation_name = aws_s3_request_type_to_operation_name_static_string(request_type);

    request->part_number = part_number;
    request->record_response_headers = (flags & AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS) != 0;
    request->has_part_size_response_body = (flags & AWS_S3_REQUEST_FLAG_PART_SIZE_RESPONSE_BODY) != 0;
    request->has_part_size_request_body = (flags & AWS_S3_REQUEST_FLAG_PART_SIZE_REQUEST_BODY) != 0;
    request->always_send = (flags & AWS_S3_REQUEST_FLAG_ALWAYS_SEND) != 0;

    return request;
}

void aws_s3_request_setup_send_data(struct aws_s3_request *request, struct aws_http_message *message) {
    AWS_PRECONDITION(request);
    AWS_PRECONDITION(message);

    if (request != NULL && request->send_data.metrics != NULL) {
        /* If there is a metrics from previous attempt, complete it now. */
        struct aws_s3_request_metrics *metric = request->send_data.metrics;
        aws_high_res_clock_get_ticks((uint64_t *)&metric->time_metrics.end_timestamp_ns);
        metric->time_metrics.total_duration_ns =
            metric->time_metrics.end_timestamp_ns - metric->time_metrics.start_timestamp_ns;

        struct aws_s3_meta_request *meta_request = request->meta_request;
        if (meta_request != NULL && meta_request->telemetry_callback != NULL) {

            aws_s3_meta_request_lock_synced_data(meta_request);
            struct aws_s3_meta_request_event event = {.type = AWS_S3_META_REQUEST_EVENT_TELEMETRY};
            event.u.telemetry.metrics = aws_s3_request_metrics_acquire(metric);
            aws_s3_meta_request_add_event_for_delivery_synced(meta_request, &event);
            aws_s3_meta_request_unlock_synced_data(meta_request);
        }
        request->send_data.metrics = aws_s3_request_metrics_release(metric);
    }
    aws_s3_request_clean_up_send_data(request);

    request->send_data.message = message;
    request->send_data.metrics = aws_s3_request_metrics_new(request->allocator, request, message);
    /* Start the timestamp */
    aws_high_res_clock_get_ticks((uint64_t *)&request->send_data.metrics->time_metrics.start_timestamp_ns);

    aws_http_message_acquire(message);
}

static void s_s3_request_clean_up_send_data_message(struct aws_s3_request *request) {
    AWS_PRECONDITION(request);

    struct aws_http_message *message = request->send_data.message;

    if (message == NULL) {
        return;
    }

    request->send_data.message = NULL;
    aws_http_message_release(message);
}

void aws_s3_request_clean_up_send_data(struct aws_s3_request *request) {
    AWS_PRECONDITION(request);
    /* The metrics should be collected and provided to user before reaching here */
    AWS_FATAL_ASSERT(request->send_data.metrics == NULL);

    s_s3_request_clean_up_send_data_message(request);

    aws_signable_destroy(request->send_data.signable);
    request->send_data.signable = NULL;
    aws_http_headers_release(request->send_data.response_headers);
    request->send_data.response_headers = NULL;

    aws_byte_buf_clean_up(&request->send_data.response_body);

    AWS_ZERO_STRUCT(request->send_data);
}

struct aws_s3_request *aws_s3_request_acquire(struct aws_s3_request *request) {
    if (request != NULL) {
        aws_ref_count_acquire(&request->ref_count);
    }
    return request;
}

struct aws_s3_request *aws_s3_request_release(struct aws_s3_request *request) {
    if (request != NULL) {
        aws_ref_count_release(&request->ref_count);
    }
    return NULL;
}

static void s_s3_request_destroy(void *user_data) {
    struct aws_s3_request *request = user_data;

    if (request == NULL) {
        return;
    }

    aws_s3_request_clean_up_send_data(request);
    aws_byte_buf_clean_up(&request->request_body);
    aws_s3_buffer_pool_release_ticket(request->meta_request->client->buffer_pool, request->ticket);
    aws_string_destroy(request->operation_name);
    aws_s3_meta_request_release(request->meta_request);

    aws_mem_release(request->allocator, request);
}

static void s_s3_request_metrics_destroy(void *arg) {
    struct aws_s3_request_metrics *metrics = arg;
    if (metrics == NULL) {
        return;
    }
    aws_http_headers_release(metrics->req_resp_info_metrics.response_headers);
    aws_string_destroy(metrics->req_resp_info_metrics.request_path_query);
    aws_string_destroy(metrics->req_resp_info_metrics.host_address);
    aws_string_destroy(metrics->req_resp_info_metrics.request_id);
    aws_string_destroy(metrics->req_resp_info_metrics.operation_name);
    aws_string_destroy(metrics->crt_info_metrics.ip_address);

    aws_mem_release(metrics->allocator, metrics);
}

struct aws_s3_request_metrics *aws_s3_request_metrics_new(
    struct aws_allocator *allocator,
    const struct aws_s3_request *request,
    const struct aws_http_message *message) {

    struct aws_s3_request_metrics *metrics = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_request_metrics));
    metrics->allocator = allocator;
    struct aws_byte_cursor out_path;
    AWS_ZERO_STRUCT(out_path);
    int err = aws_http_message_get_request_path(message, &out_path);
    /* If there is no path of the message, it should be a program error. */
    AWS_ASSERT(!err);
    metrics->req_resp_info_metrics.request_path_query = aws_string_new_from_cursor(allocator, &out_path);
    AWS_ASSERT(metrics->req_resp_info_metrics.request_path_query != NULL);

    /* Get the host header value */
    struct aws_byte_cursor host_header_value;
    AWS_ZERO_STRUCT(host_header_value);
    struct aws_http_headers *message_headers = aws_http_message_get_headers(message);
    AWS_ASSERT(message_headers);
    err = aws_http_headers_get(message_headers, g_host_header_name, &host_header_value);
    AWS_ASSERT(!err);
    metrics->req_resp_info_metrics.host_address = aws_string_new_from_cursor(allocator, &host_header_value);
    AWS_ASSERT(metrics->req_resp_info_metrics.host_address != NULL);

    metrics->req_resp_info_metrics.request_type = request->request_type;
    metrics->req_resp_info_metrics.operation_name = aws_string_new_from_string(allocator, request->operation_name);

    metrics->time_metrics.start_timestamp_ns = -1;
    metrics->time_metrics.end_timestamp_ns = -1;
    metrics->time_metrics.total_duration_ns = -1;
    metrics->time_metrics.send_start_timestamp_ns = -1;
    metrics->time_metrics.send_end_timestamp_ns = -1;
    metrics->time_metrics.sending_duration_ns = -1;
    metrics->time_metrics.receive_start_timestamp_ns = -1;
    metrics->time_metrics.receive_end_timestamp_ns = -1;
    metrics->time_metrics.receiving_duration_ns = -1;
    metrics->time_metrics.sign_start_timestamp_ns = -1;
    metrics->time_metrics.sign_end_timestamp_ns = -1;
    metrics->time_metrics.signing_duration_ns = -1;

    metrics->req_resp_info_metrics.response_status = -1;

    (void)err;
    aws_ref_count_init(&metrics->ref_count, metrics, s_s3_request_metrics_destroy);

    return metrics;
}
struct aws_s3_request_metrics *aws_s3_request_metrics_acquire(struct aws_s3_request_metrics *metrics) {
    if (!metrics) {
        return NULL;
    }

    aws_ref_count_acquire(&metrics->ref_count);
    return metrics;
}
struct aws_s3_request_metrics *aws_s3_request_metrics_release(struct aws_s3_request_metrics *metrics) {
    if (metrics != NULL) {
        aws_ref_count_release(&metrics->ref_count);
    }
    return NULL;
}

int aws_s3_request_metrics_get_request_id(
    const struct aws_s3_request_metrics *metrics,
    const struct aws_string **out_request_id) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(out_request_id);
    if (metrics->req_resp_info_metrics.request_id == NULL) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *out_request_id = metrics->req_resp_info_metrics.request_id;
    return AWS_OP_SUCCESS;
}

void aws_s3_request_metrics_get_start_timestamp_ns(const struct aws_s3_request_metrics *metrics, uint64_t *start_time) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(start_time);
    *start_time = metrics->time_metrics.start_timestamp_ns;
}

void aws_s3_request_metrics_get_end_timestamp_ns(const struct aws_s3_request_metrics *metrics, uint64_t *end_time) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(end_time);
    *end_time = metrics->time_metrics.end_timestamp_ns;
}

void aws_s3_request_metrics_get_total_duration_ns(
    const struct aws_s3_request_metrics *metrics,
    uint64_t *total_duration) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(total_duration);
    *total_duration = metrics->time_metrics.total_duration_ns;
}

int aws_s3_request_metrics_get_send_start_timestamp_ns(
    const struct aws_s3_request_metrics *metrics,
    uint64_t *send_start_time) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(send_start_time);
    if (metrics->time_metrics.send_start_timestamp_ns < 0) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *send_start_time = metrics->time_metrics.send_start_timestamp_ns;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_send_end_timestamp_ns(
    const struct aws_s3_request_metrics *metrics,
    uint64_t *send_end_time) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(send_end_time);
    if (metrics->time_metrics.send_end_timestamp_ns < 0) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *send_end_time = metrics->time_metrics.send_end_timestamp_ns;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_sending_duration_ns(
    const struct aws_s3_request_metrics *metrics,
    uint64_t *sending_duration) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(sending_duration);
    if (metrics->time_metrics.sending_duration_ns < 0) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *sending_duration = metrics->time_metrics.sending_duration_ns;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_receive_start_timestamp_ns(
    const struct aws_s3_request_metrics *metrics,
    uint64_t *receive_start_time) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(receive_start_time);
    if (metrics->time_metrics.receive_start_timestamp_ns < 0) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *receive_start_time = metrics->time_metrics.receive_start_timestamp_ns;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_receive_end_timestamp_ns(
    const struct aws_s3_request_metrics *metrics,
    uint64_t *receive_end_time) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(receive_end_time);
    if (metrics->time_metrics.receive_end_timestamp_ns < 0) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *receive_end_time = metrics->time_metrics.receive_end_timestamp_ns;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_receiving_duration_ns(
    const struct aws_s3_request_metrics *metrics,
    uint64_t *receiving_duration) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(receiving_duration);
    if (metrics->time_metrics.receiving_duration_ns < 0) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *receiving_duration = metrics->time_metrics.receiving_duration_ns;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_response_status_code(
    const struct aws_s3_request_metrics *metrics,
    int *response_status) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(response_status);
    if (metrics->req_resp_info_metrics.response_status == -1) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *response_status = metrics->req_resp_info_metrics.response_status;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_response_headers(
    const struct aws_s3_request_metrics *metrics,
    struct aws_http_headers **response_headers) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(response_headers);
    if (metrics->req_resp_info_metrics.response_headers == NULL) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *response_headers = metrics->req_resp_info_metrics.response_headers;
    return AWS_OP_SUCCESS;
}

void aws_s3_request_metrics_get_request_path_query(
    const struct aws_s3_request_metrics *metrics,
    const struct aws_string **request_path_query) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(request_path_query);
    *request_path_query = metrics->req_resp_info_metrics.request_path_query;
}

void aws_s3_request_metrics_get_host_address(
    const struct aws_s3_request_metrics *metrics,
    const struct aws_string **host_address) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(host_address);
    *host_address = metrics->req_resp_info_metrics.host_address;
}

int aws_s3_request_metrics_get_ip_address(
    const struct aws_s3_request_metrics *metrics,
    const struct aws_string **ip_address) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(ip_address);
    if (metrics->crt_info_metrics.ip_address == NULL) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *ip_address = metrics->crt_info_metrics.ip_address;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_connection_id(const struct aws_s3_request_metrics *metrics, size_t *connection_id) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(connection_id);
    if (metrics->crt_info_metrics.connection_id == NULL) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *connection_id = (size_t)metrics->crt_info_metrics.connection_id;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_thread_id(const struct aws_s3_request_metrics *metrics, aws_thread_id_t *thread_id) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(thread_id);
    if (metrics->crt_info_metrics.thread_id == 0) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *thread_id = metrics->crt_info_metrics.thread_id;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_request_stream_id(const struct aws_s3_request_metrics *metrics, uint32_t *stream_id) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(stream_id);
    if (metrics->crt_info_metrics.stream_id == 0) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *stream_id = metrics->crt_info_metrics.stream_id;
    return AWS_OP_SUCCESS;
}

int aws_s3_request_metrics_get_operation_name(
    const struct aws_s3_request_metrics *metrics,
    const struct aws_string **out_operation_name) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(out_operation_name);
    if (metrics->req_resp_info_metrics.operation_name == NULL) {
        return aws_raise_error(AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE);
    }
    *out_operation_name = metrics->req_resp_info_metrics.operation_name;
    return AWS_OP_SUCCESS;
}

void aws_s3_request_metrics_get_request_type(
    const struct aws_s3_request_metrics *metrics,
    enum aws_s3_request_type *out_request_type) {
    AWS_PRECONDITION(metrics);
    AWS_PRECONDITION(out_request_type);
    *out_request_type = metrics->req_resp_info_metrics.request_type;
}

int aws_s3_request_metrics_get_error_code(const struct aws_s3_request_metrics *metrics) {
    AWS_PRECONDITION(metrics);
    return metrics->crt_info_metrics.error_code;
}
