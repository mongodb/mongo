/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/private/s3_paginator.h>
#include <aws/s3/s3_client.h>

#include <aws/common/assert.h>
#include <aws/common/atomics.h>
#include <aws/common/byte_buf.h>
#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/common/string.h>
#include <aws/common/xml_parser.h>
#include <aws/http/request_response.h>

static const size_t s_dynamic_body_initial_buf_size = 1024;

enum operation_state {
    OS_NOT_STARTED,
    OS_INITIATED,
    OS_COMPLETED,
    OS_ERROR,
};

struct aws_s3_paginated_operation {
    struct aws_allocator *allocator;

    struct aws_string *operation_name;
    struct aws_string *result_xml_node_name;
    struct aws_string *continuation_xml_node_name;

    aws_s3_next_http_message_fn *next_http_message;
    aws_s3_on_result_node_encountered_fn *on_result_node_encountered;

    aws_s3_on_paginated_operation_cleanup_fn *on_paginated_operation_cleanup;

    void *user_data;

    struct aws_ref_count ref_count;
};

struct aws_s3_paginator {
    struct aws_allocator *allocator;
    struct aws_s3_client *client;

    /** The current, in-flight paginated request to s3. */
    struct aws_atomic_var current_request;

    struct aws_string *bucket_name;
    struct aws_string *endpoint;

    struct aws_s3_paginated_operation *operation;

    struct aws_ref_count ref_count;
    struct {
        struct aws_string *continuation_token;
        enum operation_state operation_state;
        struct aws_mutex lock;
        bool has_more_results;
    } shared_mt_state;

    struct aws_byte_buf result_body;

    aws_s3_on_page_finished_fn *on_page_finished;
    void *user_data;
};

static void s_operation_ref_count_zero_callback(void *arg) {
    struct aws_s3_paginated_operation *operation = arg;

    if (operation->on_paginated_operation_cleanup) {
        operation->on_paginated_operation_cleanup(operation->user_data);
    }

    if (operation->operation_name) {
        aws_string_destroy(operation->operation_name);
    }

    if (operation->result_xml_node_name) {
        aws_string_destroy(operation->result_xml_node_name);
    }

    if (operation->continuation_xml_node_name) {
        aws_string_destroy(operation->continuation_xml_node_name);
    }

    aws_mem_release(operation->allocator, operation);
}

static void s_paginator_ref_count_zero_callback(void *arg) {
    struct aws_s3_paginator *paginator = arg;

    aws_s3_client_release(paginator->client);
    aws_s3_paginated_operation_release(paginator->operation);

    aws_byte_buf_clean_up(&paginator->result_body);

    struct aws_s3_meta_request *previous_request = aws_atomic_exchange_ptr(&paginator->current_request, NULL);
    if (previous_request != NULL) {
        aws_s3_meta_request_release(previous_request);
    }

    if (paginator->bucket_name) {
        aws_string_destroy(paginator->bucket_name);
    }

    if (paginator->endpoint) {
        aws_string_destroy(paginator->endpoint);
    }

    if (paginator->shared_mt_state.continuation_token) {
        aws_string_destroy(paginator->shared_mt_state.continuation_token);
    }

    aws_mem_release(paginator->allocator, paginator);
}

struct aws_s3_paginator *aws_s3_initiate_paginator(
    struct aws_allocator *allocator,
    const struct aws_s3_paginator_params *params) {
    AWS_FATAL_PRECONDITION(params);
    AWS_FATAL_PRECONDITION(params->client);

    struct aws_s3_paginator *paginator = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_paginator));
    paginator->allocator = allocator;
    paginator->client = aws_s3_client_acquire(params->client);
    paginator->operation = params->operation;
    paginator->on_page_finished = params->on_page_finished_fn;
    paginator->user_data = params->user_data;

    paginator->bucket_name = aws_string_new_from_cursor(allocator, &params->bucket_name);
    paginator->endpoint = aws_string_new_from_cursor(allocator, &params->endpoint);

    aws_s3_paginated_operation_acquire(params->operation);

    aws_byte_buf_init(&paginator->result_body, allocator, s_dynamic_body_initial_buf_size);
    aws_ref_count_init(&paginator->ref_count, paginator, s_paginator_ref_count_zero_callback);
    aws_mutex_init(&paginator->shared_mt_state.lock);
    aws_atomic_init_ptr(&paginator->current_request, NULL);
    paginator->shared_mt_state.operation_state = OS_NOT_STARTED;

    return paginator;
}

void aws_s3_paginator_release(struct aws_s3_paginator *paginator) {
    if (paginator) {
        aws_ref_count_release(&paginator->ref_count);
    }
}

void aws_s3_paginator_acquire(struct aws_s3_paginator *paginator) {
    AWS_FATAL_PRECONDITION(paginator);
    aws_ref_count_acquire(&paginator->ref_count);
}

struct aws_s3_paginated_operation *aws_s3_paginated_operation_new(
    struct aws_allocator *allocator,
    const struct aws_s3_paginated_operation_params *params) {

    struct aws_s3_paginated_operation *operation =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_paginated_operation));
    operation->allocator = allocator;

    operation->operation_name = aws_string_new_from_cursor(allocator, &params->operation_name);
    operation->result_xml_node_name = aws_string_new_from_cursor(allocator, &params->result_xml_node_name);
    operation->continuation_xml_node_name =
        aws_string_new_from_cursor(allocator, &params->continuation_token_node_name);

    operation->next_http_message = params->next_message;
    operation->on_result_node_encountered = params->on_result_node_encountered_fn;
    operation->on_paginated_operation_cleanup = params->on_paginated_operation_cleanup;

    operation->user_data = params->user_data;

    aws_ref_count_init(&operation->ref_count, operation, s_operation_ref_count_zero_callback);

    return operation;
}

void aws_s3_paginated_operation_acquire(struct aws_s3_paginated_operation *operation) {
    AWS_FATAL_PRECONDITION(operation);
    aws_ref_count_acquire(&operation->ref_count);
}

void aws_s3_paginated_operation_release(struct aws_s3_paginated_operation *operation) {
    if (operation) {
        aws_ref_count_release(&operation->ref_count);
    }
}

bool aws_s3_paginator_has_more_results(const struct aws_s3_paginator *paginator) {
    AWS_PRECONDITION(paginator);
    bool has_more_results = false;
    struct aws_s3_paginator *paginator_mut = (struct aws_s3_paginator *)paginator;
    aws_mutex_lock(&paginator_mut->shared_mt_state.lock);
    has_more_results = paginator->shared_mt_state.has_more_results;
    aws_mutex_unlock(&paginator_mut->shared_mt_state.lock);
    AWS_LOGF_INFO(AWS_LS_S3_GENERAL, "has more %d", has_more_results);
    return has_more_results;
}

struct aws_string *s_paginator_get_continuation_token(const struct aws_s3_paginator *paginator) {
    AWS_PRECONDITION(paginator);
    struct aws_string *continuation_token = NULL;
    struct aws_s3_paginator *paginator_mut = (struct aws_s3_paginator *)paginator;
    aws_mutex_lock(&paginator_mut->shared_mt_state.lock);
    if (paginator->shared_mt_state.continuation_token) {
        continuation_token =
            aws_string_clone_or_reuse(paginator->allocator, paginator->shared_mt_state.continuation_token);
    }
    aws_mutex_unlock(&paginator_mut->shared_mt_state.lock);
    return continuation_token;
}

static inline int s_set_paginator_state_if_legal(
    struct aws_s3_paginator *paginator,
    enum operation_state expected,
    enum operation_state state) {
    aws_mutex_lock(&paginator->shared_mt_state.lock);
    if (paginator->shared_mt_state.operation_state != expected) {
        aws_mutex_unlock(&paginator->shared_mt_state.lock);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    paginator->shared_mt_state.operation_state = state;
    aws_mutex_unlock(&paginator->shared_mt_state.lock);
    return AWS_OP_SUCCESS;
}

/**
 * On a successful operation, this is an xml document. Just copy the buffers over until we're ready to parse (upon
 * completion) of the response body.
 */
static int s_receive_body_callback(
    struct aws_s3_meta_request *meta_request,
    const struct aws_byte_cursor *body,
    uint64_t range_start,
    void *user_data) {
    (void)range_start;
    (void)meta_request;

    struct aws_s3_paginator *paginator = user_data;

    if (body && body->len) {
        aws_byte_buf_append_dynamic(&paginator->result_body, body);
    }
    return AWS_OP_SUCCESS;
}

struct parser_wrapper {
    struct aws_s3_paginated_operation *operation;
    struct aws_string *next_continuation_token;
    bool has_more_results;
};

static int s_on_result_node_encountered(struct aws_xml_node *node, void *user_data) {

    struct parser_wrapper *wrapper = user_data;

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);

    struct aws_byte_cursor continuation_name_val =
        aws_byte_cursor_from_string(wrapper->operation->continuation_xml_node_name);
    if (aws_byte_cursor_eq_ignore_case(&node_name, &continuation_name_val)) {
        struct aws_byte_cursor continuation_token_cur;
        if (aws_xml_node_as_body(node, &continuation_token_cur) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        wrapper->next_continuation_token =
            aws_string_new_from_cursor(wrapper->operation->allocator, &continuation_token_cur);

        return AWS_OP_SUCCESS;
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "IsTruncated")) {
        struct aws_byte_cursor truncated_cur;
        if (aws_xml_node_as_body(node, &truncated_cur) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        if (aws_byte_cursor_eq_c_str_ignore_case(&truncated_cur, "true")) {
            wrapper->has_more_results = true;
        }

        return AWS_OP_SUCCESS;
    }

    return wrapper->operation->on_result_node_encountered(node, wrapper->operation->user_data);
}

static int s_on_root_node_encountered(struct aws_xml_node *node, void *user_data) {
    struct parser_wrapper *wrapper = user_data;

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    struct aws_byte_cursor result_name_val = aws_byte_cursor_from_string(wrapper->operation->result_xml_node_name);
    if (aws_byte_cursor_eq_ignore_case(&node_name, &result_name_val)) {
        return aws_xml_node_traverse(node, s_on_result_node_encountered, wrapper);
    }

    /* root element not what we expected */
    return aws_raise_error(AWS_ERROR_INVALID_XML);
}

static void s_on_request_finished(
    struct aws_s3_meta_request *meta_request,
    const struct aws_s3_meta_request_result *meta_request_result,
    void *user_data) {
    (void)meta_request;
    struct aws_s3_paginator *paginator = user_data;

    if (meta_request_result->response_status == 200) {
        /* clears previous continuation token */
        aws_mutex_lock(&paginator->shared_mt_state.lock);
        if (paginator->shared_mt_state.continuation_token) {
            aws_string_destroy(paginator->shared_mt_state.continuation_token);
            paginator->shared_mt_state.continuation_token = NULL;
            paginator->shared_mt_state.has_more_results = false;
        }
        aws_mutex_unlock(&paginator->shared_mt_state.lock);

        struct aws_byte_cursor result_body_cursor = aws_byte_cursor_from_buf(&paginator->result_body);
        struct aws_string *continuation_token = NULL;
        bool has_more_results = false;
        aws_s3_paginated_operation_on_response(
            paginator->operation, &result_body_cursor, &continuation_token, &has_more_results);

        aws_mutex_lock(&paginator->shared_mt_state.lock);

        if (paginator->shared_mt_state.continuation_token) {
            aws_string_destroy(paginator->shared_mt_state.continuation_token);
        }

        paginator->shared_mt_state.continuation_token = continuation_token;
        paginator->shared_mt_state.has_more_results = has_more_results;
        aws_mutex_unlock(&paginator->shared_mt_state.lock);

        if (has_more_results) {
            s_set_paginator_state_if_legal(paginator, OS_INITIATED, OS_NOT_STARTED);
        } else {
            s_set_paginator_state_if_legal(paginator, OS_INITIATED, OS_COMPLETED);
        }

    } else {
        s_set_paginator_state_if_legal(paginator, OS_INITIATED, OS_ERROR);
    }

    if (paginator->on_page_finished) {
        paginator->on_page_finished(paginator, meta_request_result->error_code, paginator->user_data);
    }

    /* this ref count was done right before we kicked off the request to keep the paginator object alive. Release it now
     * that the operation has completed. */
    aws_s3_paginator_release(paginator);
}

int aws_s3_paginated_operation_on_response(
    struct aws_s3_paginated_operation *operation,
    struct aws_byte_cursor *response_body,
    struct aws_string **continuation_token_out,
    bool *has_more_results_out) {

    struct parser_wrapper wrapper = {.operation = operation};

    /* we've got a full xml document now and the request succeeded, parse the document and fire all the callbacks
     * for each object and prefix. All of that happens in these three lines. */
    struct aws_xml_parser_options parser_options = {
        .doc = *response_body,
        .max_depth = 16U,
        .on_root_encountered = s_on_root_node_encountered,
        .user_data = &wrapper,
    };
    if (aws_xml_parse(operation->allocator, &parser_options) != AWS_OP_SUCCESS) {
        aws_string_destroy(wrapper.next_continuation_token);
        *continuation_token_out = NULL;
        *has_more_results_out = false;
        return AWS_OP_ERR;
    }

    *continuation_token_out = wrapper.next_continuation_token;
    *has_more_results_out = wrapper.has_more_results;
    return AWS_OP_SUCCESS;
}

int aws_s3_construct_next_paginated_request_http_message(
    struct aws_s3_paginated_operation *operation,
    struct aws_byte_cursor *continuation_token,
    struct aws_http_message **out_message) {
    return operation->next_http_message(continuation_token, operation->user_data, out_message);
}

int aws_s3_paginator_continue(struct aws_s3_paginator *paginator, const struct aws_signing_config_aws *signing_config) {
    AWS_PRECONDITION(paginator);
    AWS_PRECONDITION(signing_config);

    int re_code = AWS_OP_ERR;

    if (s_set_paginator_state_if_legal(paginator, OS_NOT_STARTED, OS_INITIATED)) {
        return re_code;
    }

    struct aws_http_message *paginated_request_message = NULL;
    struct aws_string *continuation_string = NULL;
    struct aws_byte_buf host_buf;
    AWS_ZERO_STRUCT(host_buf);

    struct aws_byte_cursor host_cur = aws_byte_cursor_from_string(paginator->bucket_name);
    struct aws_byte_cursor period_cur = aws_byte_cursor_from_c_str(".");
    struct aws_byte_cursor endpoint_val = aws_byte_cursor_from_string(paginator->endpoint);
    if (aws_byte_buf_init_copy_from_cursor(&host_buf, paginator->allocator, host_cur) ||
        aws_byte_buf_append_dynamic(&host_buf, &period_cur) || aws_byte_buf_append_dynamic(&host_buf, &endpoint_val)) {
        goto done;
    }

    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("host"),
        .value = aws_byte_cursor_from_buf(&host_buf),
    };

    continuation_string = s_paginator_get_continuation_token(paginator);
    struct aws_byte_cursor continuation_cursor;
    AWS_ZERO_STRUCT(continuation_cursor);
    struct aws_byte_cursor *continuation = NULL;
    if (continuation_string) {
        continuation_cursor = aws_byte_cursor_from_string(continuation_string);
        continuation = &continuation_cursor;
    }
    if (paginator->operation->next_http_message(
            continuation, paginator->operation->user_data, &paginated_request_message)) {
        goto done;
    }

    if (aws_http_message_add_header(paginated_request_message, host_header)) {
        goto done;
    }

    struct aws_s3_meta_request_options request_options = {
        .user_data = paginator,
        .signing_config = (struct aws_signing_config_aws *)signing_config,
        .type = AWS_S3_META_REQUEST_TYPE_DEFAULT,
        .operation_name = aws_byte_cursor_from_string(paginator->operation->operation_name),
        .body_callback = s_receive_body_callback,
        .finish_callback = s_on_request_finished,
        .message = paginated_request_message,
    };

    /* re-use the current buffer. */
    aws_byte_buf_reset(&paginator->result_body, false);

    /* we're kicking off an asynchronous request. ref-count the paginator to keep it alive until we finish. */
    aws_s3_paginator_acquire(paginator);

    struct aws_s3_meta_request *previous_request = aws_atomic_exchange_ptr(&paginator->current_request, NULL);
    if (previous_request != NULL) {
        /* release request from previous page */
        aws_s3_meta_request_release(previous_request);
    }

    struct aws_s3_meta_request *new_request = aws_s3_client_make_meta_request(paginator->client, &request_options);
    aws_atomic_store_ptr(&paginator->current_request, new_request);

    if (new_request == NULL) {
        s_set_paginator_state_if_legal(paginator, OS_INITIATED, OS_ERROR);
        goto done;
    }

    re_code = AWS_OP_SUCCESS;
done:
    aws_http_message_release(paginated_request_message);
    aws_string_destroy(continuation_string);
    aws_byte_buf_clean_up(&host_buf);
    return re_code;
}
