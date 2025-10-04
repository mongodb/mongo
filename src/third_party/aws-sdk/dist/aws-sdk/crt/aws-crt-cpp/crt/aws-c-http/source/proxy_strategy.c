/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/proxy.h>

#include <aws/common/encoding.h>
#include <aws/common/string.h>
#include <aws/http/private/proxy_impl.h>

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4221)
#endif /* _MSC_VER */

struct aws_http_proxy_negotiator *aws_http_proxy_negotiator_acquire(
    struct aws_http_proxy_negotiator *proxy_negotiator) {
    if (proxy_negotiator != NULL) {
        aws_ref_count_acquire(&proxy_negotiator->ref_count);
    }

    return proxy_negotiator;
}

void aws_http_proxy_negotiator_release(struct aws_http_proxy_negotiator *proxy_negotiator) {
    if (proxy_negotiator != NULL) {
        aws_ref_count_release(&proxy_negotiator->ref_count);
    }
}

struct aws_http_proxy_negotiator *aws_http_proxy_strategy_create_negotiator(
    struct aws_http_proxy_strategy *strategy,
    struct aws_allocator *allocator) {
    if (strategy == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    return strategy->vtable->create_negotiator(strategy, allocator);
}

enum aws_http_proxy_negotiation_retry_directive aws_http_proxy_negotiator_get_retry_directive(
    struct aws_http_proxy_negotiator *proxy_negotiator) {
    if (proxy_negotiator != NULL) {
        if (proxy_negotiator->strategy_vtable.tunnelling_vtable->get_retry_directive != NULL) {
            return proxy_negotiator->strategy_vtable.tunnelling_vtable->get_retry_directive(proxy_negotiator);
        }
    }

    return AWS_HPNRD_STOP;
}

struct aws_http_proxy_strategy *aws_http_proxy_strategy_acquire(struct aws_http_proxy_strategy *proxy_strategy) {
    if (proxy_strategy != NULL) {
        aws_ref_count_acquire(&proxy_strategy->ref_count);
    }

    return proxy_strategy;
}

void aws_http_proxy_strategy_release(struct aws_http_proxy_strategy *proxy_strategy) {
    if (proxy_strategy != NULL) {
        aws_ref_count_release(&proxy_strategy->ref_count);
    }
}

/*****************************************************************************************************************/

enum proxy_negotiator_connect_state {
    AWS_PNCS_READY,
    AWS_PNCS_IN_PROGRESS,
    AWS_PNCS_SUCCESS,
    AWS_PNCS_FAILURE,
};

/* Functions for basic auth strategy */

struct aws_http_proxy_strategy_basic_auth {
    struct aws_allocator *allocator;
    struct aws_string *user_name;
    struct aws_string *password;
    struct aws_http_proxy_strategy strategy_base;
};

static void s_destroy_basic_auth_strategy(struct aws_http_proxy_strategy *proxy_strategy) {
    struct aws_http_proxy_strategy_basic_auth *basic_auth_strategy = proxy_strategy->impl;

    aws_string_destroy(basic_auth_strategy->user_name);
    aws_string_destroy(basic_auth_strategy->password);

    aws_mem_release(basic_auth_strategy->allocator, basic_auth_strategy);
}

struct aws_http_proxy_negotiator_basic_auth {
    struct aws_allocator *allocator;

    struct aws_http_proxy_strategy *strategy;

    enum proxy_negotiator_connect_state connect_state;

    struct aws_http_proxy_negotiator negotiator_base;
};

static void s_destroy_basic_auth_negotiator(struct aws_http_proxy_negotiator *proxy_negotiator) {
    struct aws_http_proxy_negotiator_basic_auth *basic_auth_negotiator = proxy_negotiator->impl;

    aws_http_proxy_strategy_release(basic_auth_negotiator->strategy);

    aws_mem_release(basic_auth_negotiator->allocator, basic_auth_negotiator);
}

AWS_STATIC_STRING_FROM_LITERAL(s_proxy_authorization_header_name, "Proxy-Authorization");
AWS_STATIC_STRING_FROM_LITERAL(s_proxy_authorization_header_basic_prefix, "Basic ");

/*
 * Adds a proxy authentication header based on the basic authentication mode, rfc7617
 */
static int s_add_basic_proxy_authentication_header(
    struct aws_allocator *allocator,
    struct aws_http_message *request,
    struct aws_http_proxy_negotiator_basic_auth *basic_auth_negotiator) {

    struct aws_byte_buf base64_input_value;
    AWS_ZERO_STRUCT(base64_input_value);

    struct aws_byte_buf header_value;
    AWS_ZERO_STRUCT(header_value);

    int result = AWS_OP_ERR;

    struct aws_http_proxy_strategy_basic_auth *basic_auth_strategy = basic_auth_negotiator->strategy->impl;

    if (aws_byte_buf_init(
            &base64_input_value,
            allocator,
            basic_auth_strategy->user_name->len + basic_auth_strategy->password->len + 1)) {
        goto done;
    }

    /* First build a buffer with "username:password" in it */
    struct aws_byte_cursor username_cursor = aws_byte_cursor_from_string(basic_auth_strategy->user_name);
    if (aws_byte_buf_append(&base64_input_value, &username_cursor)) {
        goto done;
    }

    struct aws_byte_cursor colon_cursor = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":");
    if (aws_byte_buf_append(&base64_input_value, &colon_cursor)) {
        goto done;
    }

    struct aws_byte_cursor password_cursor = aws_byte_cursor_from_string(basic_auth_strategy->password);
    if (aws_byte_buf_append(&base64_input_value, &password_cursor)) {
        goto done;
    }

    struct aws_byte_cursor base64_source_cursor =
        aws_byte_cursor_from_array(base64_input_value.buffer, base64_input_value.len);

    /* Figure out how much room we need in our final header value buffer */
    size_t required_size = 0;
    if (aws_base64_compute_encoded_len(base64_source_cursor.len, &required_size)) {
        goto done;
    }

    required_size += s_proxy_authorization_header_basic_prefix->len + 1;
    if (aws_byte_buf_init(&header_value, allocator, required_size)) {
        goto done;
    }

    /* Build the final header value by appending the authorization type and the base64 encoding string together */
    struct aws_byte_cursor basic_prefix = aws_byte_cursor_from_string(s_proxy_authorization_header_basic_prefix);
    if (aws_byte_buf_append_dynamic(&header_value, &basic_prefix)) {
        goto done;
    }

    if (aws_base64_encode(&base64_source_cursor, &header_value)) {
        goto done;
    }

    struct aws_http_header header = {
        .name = aws_byte_cursor_from_string(s_proxy_authorization_header_name),
        .value = aws_byte_cursor_from_array(header_value.buffer, header_value.len),
    };

    if (aws_http_message_add_header(request, header)) {
        goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    aws_byte_buf_clean_up(&header_value);
    aws_byte_buf_clean_up(&base64_input_value);

    return result;
}

int s_basic_auth_forward_add_header(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message) {
    struct aws_http_proxy_negotiator_basic_auth *basic_auth_negotiator = proxy_negotiator->impl;

    return s_add_basic_proxy_authentication_header(basic_auth_negotiator->allocator, message, basic_auth_negotiator);
}

void s_basic_auth_tunnel_add_header(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message,
    aws_http_proxy_negotiation_terminate_fn *negotiation_termination_callback,
    aws_http_proxy_negotiation_http_request_forward_fn *negotiation_http_request_forward_callback,
    void *internal_proxy_user_data) {

    struct aws_http_proxy_negotiator_basic_auth *basic_auth_negotiator = proxy_negotiator->impl;
    if (basic_auth_negotiator->connect_state != AWS_PNCS_READY) {
        negotiation_termination_callback(message, AWS_ERROR_HTTP_PROXY_CONNECT_FAILED, internal_proxy_user_data);
        return;
    }

    basic_auth_negotiator->connect_state = AWS_PNCS_IN_PROGRESS;

    if (s_add_basic_proxy_authentication_header(basic_auth_negotiator->allocator, message, basic_auth_negotiator)) {
        negotiation_termination_callback(message, aws_last_error(), internal_proxy_user_data);
        return;
    }

    negotiation_http_request_forward_callback(message, internal_proxy_user_data);
}

static int s_basic_auth_on_connect_status(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_status_code status_code) {
    struct aws_http_proxy_negotiator_basic_auth *basic_auth_negotiator = proxy_negotiator->impl;

    if (basic_auth_negotiator->connect_state == AWS_PNCS_IN_PROGRESS) {
        if (AWS_HTTP_STATUS_CODE_200_OK != status_code) {
            basic_auth_negotiator->connect_state = AWS_PNCS_FAILURE;
        } else {
            basic_auth_negotiator->connect_state = AWS_PNCS_SUCCESS;
        }
    }

    return AWS_OP_SUCCESS;
}

static struct aws_http_proxy_negotiator_forwarding_vtable s_basic_auth_proxy_negotiator_forwarding_vtable = {
    .forward_request_transform = s_basic_auth_forward_add_header,
};

static struct aws_http_proxy_negotiator_tunnelling_vtable s_basic_auth_proxy_negotiator_tunneling_vtable = {
    .on_status_callback = s_basic_auth_on_connect_status,
    .connect_request_transform = s_basic_auth_tunnel_add_header,
};

static struct aws_http_proxy_negotiator *s_create_basic_auth_negotiator(
    struct aws_http_proxy_strategy *proxy_strategy,
    struct aws_allocator *allocator) {
    if (proxy_strategy == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_negotiator_basic_auth *basic_auth_negotiator =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_negotiator_basic_auth));
    if (basic_auth_negotiator == NULL) {
        return NULL;
    }

    basic_auth_negotiator->allocator = allocator;
    basic_auth_negotiator->connect_state = AWS_PNCS_READY;
    basic_auth_negotiator->negotiator_base.impl = basic_auth_negotiator;
    aws_ref_count_init(
        &basic_auth_negotiator->negotiator_base.ref_count,
        &basic_auth_negotiator->negotiator_base,
        (aws_simple_completion_callback *)s_destroy_basic_auth_negotiator);

    if (proxy_strategy->proxy_connection_type == AWS_HPCT_HTTP_FORWARD) {
        basic_auth_negotiator->negotiator_base.strategy_vtable.forwarding_vtable =
            &s_basic_auth_proxy_negotiator_forwarding_vtable;
    } else {
        basic_auth_negotiator->negotiator_base.strategy_vtable.tunnelling_vtable =
            &s_basic_auth_proxy_negotiator_tunneling_vtable;
    }

    basic_auth_negotiator->strategy = aws_http_proxy_strategy_acquire(proxy_strategy);

    return &basic_auth_negotiator->negotiator_base;
}

static struct aws_http_proxy_strategy_vtable s_basic_auth_proxy_strategy_vtable = {
    .create_negotiator = s_create_basic_auth_negotiator,
};

struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_basic_auth(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_basic_auth_options *config) {
    if (config == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (config->proxy_connection_type != AWS_HPCT_HTTP_FORWARD &&
        config->proxy_connection_type != AWS_HPCT_HTTP_TUNNEL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_strategy_basic_auth *basic_auth_strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_strategy_basic_auth));
    if (basic_auth_strategy == NULL) {
        return NULL;
    }

    basic_auth_strategy->strategy_base.impl = basic_auth_strategy;
    basic_auth_strategy->strategy_base.vtable = &s_basic_auth_proxy_strategy_vtable;
    basic_auth_strategy->allocator = allocator;
    basic_auth_strategy->strategy_base.proxy_connection_type = config->proxy_connection_type;
    aws_ref_count_init(
        &basic_auth_strategy->strategy_base.ref_count,
        &basic_auth_strategy->strategy_base,
        (aws_simple_completion_callback *)s_destroy_basic_auth_strategy);

    basic_auth_strategy->user_name = aws_string_new_from_cursor(allocator, &config->user_name);
    if (basic_auth_strategy->user_name == NULL) {
        goto on_error;
    }

    basic_auth_strategy->password = aws_string_new_from_cursor(allocator, &config->password);
    if (basic_auth_strategy->password == NULL) {
        goto on_error;
    }

    return &basic_auth_strategy->strategy_base;

on_error:

    aws_http_proxy_strategy_release(&basic_auth_strategy->strategy_base);

    return NULL;
}

/*****************************************************************************************************************/

struct aws_http_proxy_strategy_one_time_identity {
    struct aws_allocator *allocator;

    struct aws_http_proxy_strategy strategy_base;
};

struct aws_http_proxy_negotiator_one_time_identity {
    struct aws_allocator *allocator;

    enum proxy_negotiator_connect_state connect_state;

    struct aws_http_proxy_negotiator negotiator_base;
};

static void s_destroy_one_time_identity_negotiator(struct aws_http_proxy_negotiator *proxy_negotiator) {
    struct aws_http_proxy_negotiator_one_time_identity *identity_negotiator = proxy_negotiator->impl;

    aws_mem_release(identity_negotiator->allocator, identity_negotiator);
}

void s_one_time_identity_connect_transform(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message,
    aws_http_proxy_negotiation_terminate_fn *negotiation_termination_callback,
    aws_http_proxy_negotiation_http_request_forward_fn *negotiation_http_request_forward_callback,
    void *internal_proxy_user_data) {

    struct aws_http_proxy_negotiator_one_time_identity *one_time_identity_negotiator = proxy_negotiator->impl;
    if (one_time_identity_negotiator->connect_state != AWS_PNCS_READY) {
        negotiation_termination_callback(message, AWS_ERROR_HTTP_PROXY_CONNECT_FAILED, internal_proxy_user_data);
        return;
    }

    one_time_identity_negotiator->connect_state = AWS_PNCS_IN_PROGRESS;
    negotiation_http_request_forward_callback(message, internal_proxy_user_data);
}

static int s_one_time_identity_on_connect_status(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_status_code status_code) {
    struct aws_http_proxy_negotiator_one_time_identity *one_time_identity_negotiator = proxy_negotiator->impl;

    if (one_time_identity_negotiator->connect_state == AWS_PNCS_IN_PROGRESS) {
        if (AWS_HTTP_STATUS_CODE_200_OK != status_code) {
            one_time_identity_negotiator->connect_state = AWS_PNCS_FAILURE;
        } else {
            one_time_identity_negotiator->connect_state = AWS_PNCS_SUCCESS;
        }
    }

    return AWS_OP_SUCCESS;
}

static struct aws_http_proxy_negotiator_tunnelling_vtable s_one_time_identity_proxy_negotiator_tunneling_vtable = {
    .on_status_callback = s_one_time_identity_on_connect_status,
    .connect_request_transform = s_one_time_identity_connect_transform,
};

static struct aws_http_proxy_negotiator *s_create_one_time_identity_negotiator(
    struct aws_http_proxy_strategy *proxy_strategy,
    struct aws_allocator *allocator) {
    if (proxy_strategy == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_negotiator_one_time_identity *identity_negotiator =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_negotiator_one_time_identity));
    if (identity_negotiator == NULL) {
        return NULL;
    }

    identity_negotiator->allocator = allocator;
    identity_negotiator->connect_state = AWS_PNCS_READY;
    identity_negotiator->negotiator_base.impl = identity_negotiator;
    aws_ref_count_init(
        &identity_negotiator->negotiator_base.ref_count,
        &identity_negotiator->negotiator_base,
        (aws_simple_completion_callback *)s_destroy_one_time_identity_negotiator);

    identity_negotiator->negotiator_base.strategy_vtable.tunnelling_vtable =
        &s_one_time_identity_proxy_negotiator_tunneling_vtable;

    return &identity_negotiator->negotiator_base;
}

static struct aws_http_proxy_strategy_vtable s_one_time_identity_proxy_strategy_vtable = {
    .create_negotiator = s_create_one_time_identity_negotiator,
};

static void s_destroy_one_time_identity_strategy(struct aws_http_proxy_strategy *proxy_strategy) {
    struct aws_http_proxy_strategy_one_time_identity *identity_strategy = proxy_strategy->impl;

    aws_mem_release(identity_strategy->allocator, identity_strategy);
}

struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_one_time_identity(
    struct aws_allocator *allocator) {
    if (allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_strategy_one_time_identity *identity_strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_strategy_one_time_identity));
    if (identity_strategy == NULL) {
        return NULL;
    }

    identity_strategy->strategy_base.impl = identity_strategy;
    identity_strategy->strategy_base.vtable = &s_one_time_identity_proxy_strategy_vtable;
    identity_strategy->strategy_base.proxy_connection_type = AWS_HPCT_HTTP_TUNNEL;
    identity_strategy->allocator = allocator;

    aws_ref_count_init(
        &identity_strategy->strategy_base.ref_count,
        &identity_strategy->strategy_base,
        (aws_simple_completion_callback *)s_destroy_one_time_identity_strategy);

    return &identity_strategy->strategy_base;
}

/******************************************************************************************************************/

struct aws_http_proxy_strategy_forwarding_identity {
    struct aws_allocator *allocator;

    struct aws_http_proxy_strategy strategy_base;
};

struct aws_http_proxy_negotiator_forwarding_identity {
    struct aws_allocator *allocator;

    struct aws_http_proxy_negotiator negotiator_base;
};

static void s_destroy_forwarding_identity_negotiator(struct aws_http_proxy_negotiator *proxy_negotiator) {
    struct aws_http_proxy_negotiator_forwarding_identity *identity_negotiator = proxy_negotiator->impl;

    aws_mem_release(identity_negotiator->allocator, identity_negotiator);
}

int s_forwarding_identity_connect_transform(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message) {

    (void)message;
    (void)proxy_negotiator;

    return AWS_OP_SUCCESS;
}

static struct aws_http_proxy_negotiator_forwarding_vtable s_forwarding_identity_proxy_negotiator_tunneling_vtable = {
    .forward_request_transform = s_forwarding_identity_connect_transform,
};

static struct aws_http_proxy_negotiator *s_create_forwarding_identity_negotiator(
    struct aws_http_proxy_strategy *proxy_strategy,
    struct aws_allocator *allocator) {
    if (proxy_strategy == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_negotiator_forwarding_identity *identity_negotiator =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_negotiator_forwarding_identity));
    if (identity_negotiator == NULL) {
        return NULL;
    }

    identity_negotiator->allocator = allocator;
    identity_negotiator->negotiator_base.impl = identity_negotiator;
    aws_ref_count_init(
        &identity_negotiator->negotiator_base.ref_count,
        &identity_negotiator->negotiator_base,
        (aws_simple_completion_callback *)s_destroy_forwarding_identity_negotiator);

    identity_negotiator->negotiator_base.strategy_vtable.forwarding_vtable =
        &s_forwarding_identity_proxy_negotiator_tunneling_vtable;

    return &identity_negotiator->negotiator_base;
}

static struct aws_http_proxy_strategy_vtable s_forwarding_identity_strategy_vtable = {
    .create_negotiator = s_create_forwarding_identity_negotiator,
};

static void s_destroy_forwarding_identity_strategy(struct aws_http_proxy_strategy *proxy_strategy) {
    struct aws_http_proxy_strategy_forwarding_identity *identity_strategy = proxy_strategy->impl;

    aws_mem_release(identity_strategy->allocator, identity_strategy);
}

struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_forwarding_identity(struct aws_allocator *allocator) {
    if (allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_strategy_forwarding_identity *identity_strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_strategy_forwarding_identity));
    if (identity_strategy == NULL) {
        return NULL;
    }

    identity_strategy->strategy_base.impl = identity_strategy;
    identity_strategy->strategy_base.vtable = &s_forwarding_identity_strategy_vtable;
    identity_strategy->strategy_base.proxy_connection_type = AWS_HPCT_HTTP_FORWARD;
    identity_strategy->allocator = allocator;

    aws_ref_count_init(
        &identity_strategy->strategy_base.ref_count,
        &identity_strategy->strategy_base,
        (aws_simple_completion_callback *)s_destroy_forwarding_identity_strategy);

    return &identity_strategy->strategy_base;
}

/******************************************************************************************************************/
/* kerberos */

AWS_STATIC_STRING_FROM_LITERAL(s_proxy_authorization_header_kerberos_prefix, "Negotiate ");

struct aws_http_proxy_strategy_tunneling_kerberos {
    struct aws_allocator *allocator;

    aws_http_proxy_negotiation_get_token_sync_fn *get_token;

    void *get_token_user_data;

    struct aws_http_proxy_strategy strategy_base;
};

struct aws_http_proxy_negotiator_tunneling_kerberos {
    struct aws_allocator *allocator;

    struct aws_http_proxy_strategy *strategy;

    enum proxy_negotiator_connect_state connect_state;

    /*
     * ToDo: make adaptive and add any state needed here
     *
     * Likely things include response code (from the vanilla CONNECT) and the appropriate headers in
     * the response
     */

    struct aws_http_proxy_negotiator negotiator_base;
};

/*
 * Adds a proxy authentication header based on the user kerberos authentication token
 * This uses a token that is already base64 encoded
 */
static int s_add_kerberos_proxy_usertoken_authentication_header(
    struct aws_allocator *allocator,
    struct aws_http_message *request,
    struct aws_byte_cursor user_token) {

    struct aws_byte_buf header_value;
    AWS_ZERO_STRUCT(header_value);

    int result = AWS_OP_ERR;

    if (aws_byte_buf_init(
            &header_value, allocator, s_proxy_authorization_header_kerberos_prefix->len + user_token.len)) {
        goto done;
    }

    /* First append proxy authorization header kerberos prefix */
    struct aws_byte_cursor auth_header_cursor =
        aws_byte_cursor_from_string(s_proxy_authorization_header_kerberos_prefix);
    if (aws_byte_buf_append(&header_value, &auth_header_cursor)) {
        goto done;
    }

    /* Append token to it */
    if (aws_byte_buf_append(&header_value, &user_token)) {
        goto done;
    }

    struct aws_http_header header = {
        .name = aws_byte_cursor_from_string(s_proxy_authorization_header_name),
        .value = aws_byte_cursor_from_array(header_value.buffer, header_value.len),
    };

    if (aws_http_message_add_header(request, header)) {
        goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    aws_byte_buf_clean_up(&header_value);
    return result;
}

static void s_kerberos_tunnel_transform_connect(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message,
    aws_http_proxy_negotiation_terminate_fn *negotiation_termination_callback,
    aws_http_proxy_negotiation_http_request_forward_fn *negotiation_http_request_forward_callback,
    void *internal_proxy_user_data) {
    struct aws_http_proxy_negotiator_tunneling_kerberos *kerberos_negotiator = proxy_negotiator->impl;
    struct aws_http_proxy_strategy_tunneling_kerberos *kerberos_strategy = kerberos_negotiator->strategy->impl;

    int result = AWS_OP_ERR;
    int error_code = AWS_ERROR_SUCCESS;
    struct aws_string *kerberos_token = NULL;

    if (kerberos_negotiator->connect_state == AWS_PNCS_FAILURE) {
        error_code = AWS_ERROR_HTTP_PROXY_CONNECT_FAILED;
        goto done;
    }

    if (kerberos_negotiator->connect_state != AWS_PNCS_READY) {
        error_code = AWS_ERROR_INVALID_STATE;
        goto done;
    }

    kerberos_negotiator->connect_state = AWS_PNCS_IN_PROGRESS;

    kerberos_token = kerberos_strategy->get_token(kerberos_strategy->get_token_user_data, &error_code);
    if (kerberos_token == NULL || error_code != AWS_ERROR_SUCCESS) {
        goto done;
    }

    /*transform the header with proxy authenticate:Negotiate and kerberos token*/
    if (s_add_kerberos_proxy_usertoken_authentication_header(
            kerberos_negotiator->allocator, message, aws_byte_cursor_from_string(kerberos_token))) {
        error_code = aws_last_error();
        goto done;
    }

    kerberos_negotiator->connect_state = AWS_PNCS_IN_PROGRESS;
    result = AWS_OP_SUCCESS;

done:

    if (result != AWS_OP_SUCCESS) {
        if (error_code == AWS_ERROR_SUCCESS) {
            error_code = AWS_ERROR_UNKNOWN;
        }
        negotiation_termination_callback(message, error_code, internal_proxy_user_data);
    } else {
        negotiation_http_request_forward_callback(message, internal_proxy_user_data);
    }

    aws_string_destroy(kerberos_token);
}

static int s_kerberos_on_incoming_header_adaptive(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers) {

    struct aws_http_proxy_negotiator_tunneling_kerberos *kerberos_negotiator = proxy_negotiator->impl;
    (void)kerberos_negotiator;
    (void)header_block;
    (void)header_array;
    (void)num_headers;

    /* TODO: process vanilla CONNECT response headers here to improve usage/application */

    return AWS_OP_SUCCESS;
}

static int s_kerberos_on_connect_status(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_status_code status_code) {

    struct aws_http_proxy_negotiator_tunneling_kerberos *kerberos_negotiator = proxy_negotiator->impl;

    /* TODO: process status code of vanilla CONNECT request here to improve usage/application */

    if (kerberos_negotiator->connect_state == AWS_PNCS_IN_PROGRESS) {
        if (AWS_HTTP_STATUS_CODE_200_OK != status_code) {
            kerberos_negotiator->connect_state = AWS_PNCS_FAILURE;
        } else {
            kerberos_negotiator->connect_state = AWS_PNCS_SUCCESS;
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_kerberos_on_incoming_body(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    const struct aws_byte_cursor *data) {

    struct aws_http_proxy_negotiator_tunneling_kerberos *kerberos_negotiator = proxy_negotiator->impl;
    (void)kerberos_negotiator;
    (void)data;

    return AWS_OP_SUCCESS;
}

static struct aws_http_proxy_negotiator_tunnelling_vtable s_tunneling_kerberos_proxy_negotiator_tunneling_vtable = {
    .on_incoming_body_callback = s_kerberos_on_incoming_body,
    .on_incoming_headers_callback = s_kerberos_on_incoming_header_adaptive,
    .on_status_callback = s_kerberos_on_connect_status,
    .connect_request_transform = s_kerberos_tunnel_transform_connect,
};

static void s_destroy_tunneling_kerberos_negotiator(struct aws_http_proxy_negotiator *proxy_negotiator) {
    struct aws_http_proxy_negotiator_tunneling_kerberos *kerberos_negotiator = proxy_negotiator->impl;

    aws_http_proxy_strategy_release(kerberos_negotiator->strategy);

    aws_mem_release(kerberos_negotiator->allocator, kerberos_negotiator);
}

static struct aws_http_proxy_negotiator *s_create_tunneling_kerberos_negotiator(
    struct aws_http_proxy_strategy *proxy_strategy,
    struct aws_allocator *allocator) {
    if (proxy_strategy == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_negotiator_tunneling_kerberos *kerberos_negotiator =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_negotiator_tunneling_kerberos));
    if (kerberos_negotiator == NULL) {
        return NULL;
    }

    kerberos_negotiator->allocator = allocator;
    kerberos_negotiator->negotiator_base.impl = kerberos_negotiator;
    aws_ref_count_init(
        &kerberos_negotiator->negotiator_base.ref_count,
        &kerberos_negotiator->negotiator_base,
        (aws_simple_completion_callback *)s_destroy_tunneling_kerberos_negotiator);

    kerberos_negotiator->negotiator_base.strategy_vtable.tunnelling_vtable =
        &s_tunneling_kerberos_proxy_negotiator_tunneling_vtable;

    kerberos_negotiator->strategy = aws_http_proxy_strategy_acquire(proxy_strategy);

    return &kerberos_negotiator->negotiator_base;
}

static struct aws_http_proxy_strategy_vtable s_tunneling_kerberos_strategy_vtable = {
    .create_negotiator = s_create_tunneling_kerberos_negotiator,
};

static void s_destroy_tunneling_kerberos_strategy(struct aws_http_proxy_strategy *proxy_strategy) {
    struct aws_http_proxy_strategy_tunneling_kerberos *kerberos_strategy = proxy_strategy->impl;

    aws_mem_release(kerberos_strategy->allocator, kerberos_strategy);
}

struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_kerberos(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_kerberos_options *config) {

    if (allocator == NULL || config == NULL || config->get_token == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_strategy_tunneling_kerberos *kerberos_strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_strategy_tunneling_kerberos));
    if (kerberos_strategy == NULL) {
        return NULL;
    }

    kerberos_strategy->strategy_base.impl = kerberos_strategy;
    kerberos_strategy->strategy_base.vtable = &s_tunneling_kerberos_strategy_vtable;
    kerberos_strategy->strategy_base.proxy_connection_type = AWS_HPCT_HTTP_TUNNEL;
    kerberos_strategy->allocator = allocator;

    aws_ref_count_init(
        &kerberos_strategy->strategy_base.ref_count,
        &kerberos_strategy->strategy_base,
        (aws_simple_completion_callback *)s_destroy_tunneling_kerberos_strategy);

    kerberos_strategy->get_token = config->get_token;
    kerberos_strategy->get_token_user_data = config->get_token_user_data;

    return &kerberos_strategy->strategy_base;
}

/******************************************************************************************************************/

struct aws_http_proxy_strategy_tunneling_ntlm {
    struct aws_allocator *allocator;

    aws_http_proxy_negotiation_get_token_sync_fn *get_token;

    aws_http_proxy_negotiation_get_challenge_token_sync_fn *get_challenge_token;

    void *get_challenge_token_user_data;

    struct aws_http_proxy_strategy strategy_base;
};

struct aws_http_proxy_negotiator_tunneling_ntlm {
    struct aws_allocator *allocator;

    struct aws_http_proxy_strategy *strategy;

    enum proxy_negotiator_connect_state connect_state;

    struct aws_string *challenge_token;

    struct aws_http_proxy_negotiator negotiator_base;
};

AWS_STATIC_STRING_FROM_LITERAL(s_proxy_authorization_header_ntlm_prefix, "NTLM ");

/*
 * Adds a proxy authentication header based on ntlm credential or response provided by user
 */
static int s_add_ntlm_proxy_usertoken_authentication_header(
    struct aws_allocator *allocator,
    struct aws_http_message *request,
    struct aws_byte_cursor credential_response) {

    struct aws_byte_buf header_value;
    AWS_ZERO_STRUCT(header_value);

    int result = AWS_OP_ERR;

    if (aws_byte_buf_init(
            &header_value, allocator, s_proxy_authorization_header_ntlm_prefix->len + credential_response.len)) {
        goto done;
    }

    /* First append proxy authorization header prefix */
    struct aws_byte_cursor auth_header_cursor = aws_byte_cursor_from_string(s_proxy_authorization_header_ntlm_prefix);
    if (aws_byte_buf_append(&header_value, &auth_header_cursor)) {
        goto done;
    }

    /* Append the credential response to it; assumes already encoded properly (base64) */
    if (aws_byte_buf_append(&header_value, &credential_response)) {
        goto done;
    }

    struct aws_http_header header = {
        .name = aws_byte_cursor_from_string(s_proxy_authorization_header_name),
        .value = aws_byte_cursor_from_array(header_value.buffer, header_value.len),
    };

    if (aws_http_message_add_header(request, header)) {
        goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    aws_byte_buf_clean_up(&header_value);
    return result;
}

static void s_ntlm_tunnel_transform_connect(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message,
    aws_http_proxy_negotiation_terminate_fn *negotiation_termination_callback,
    aws_http_proxy_negotiation_http_request_forward_fn *negotiation_http_request_forward_callback,
    void *internal_proxy_user_data) {

    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_negotiator = proxy_negotiator->impl;
    struct aws_http_proxy_strategy_tunneling_ntlm *ntlm_strategy = ntlm_negotiator->strategy->impl;

    int result = AWS_OP_ERR;
    int error_code = AWS_ERROR_SUCCESS;
    struct aws_string *challenge_answer_token = NULL;
    struct aws_byte_cursor challenge_token;
    AWS_ZERO_STRUCT(challenge_token);

    if (ntlm_negotiator->connect_state == AWS_PNCS_FAILURE) {
        error_code = AWS_ERROR_HTTP_PROXY_CONNECT_FAILED;
        goto done;
    }

    if (ntlm_negotiator->connect_state != AWS_PNCS_READY) {
        error_code = AWS_ERROR_INVALID_STATE;
        goto done;
    }

    if (ntlm_negotiator->challenge_token == NULL) {
        error_code = AWS_ERROR_HTTP_PROXY_STRATEGY_NTLM_CHALLENGE_TOKEN_MISSING;
        goto done;
    }

    ntlm_negotiator->connect_state = AWS_PNCS_IN_PROGRESS;
    challenge_token = aws_byte_cursor_from_string(ntlm_negotiator->challenge_token);
    challenge_answer_token =
        ntlm_strategy->get_challenge_token(ntlm_strategy->get_challenge_token_user_data, &challenge_token, &error_code);

    if (challenge_answer_token == NULL || error_code != AWS_ERROR_SUCCESS) {
        goto done;
    }

    /*transform the header with proxy authenticate:Negotiate and kerberos token*/
    if (s_add_ntlm_proxy_usertoken_authentication_header(
            ntlm_negotiator->allocator, message, aws_byte_cursor_from_string(challenge_answer_token))) {
        error_code = aws_last_error();
        goto done;
    }

    ntlm_negotiator->connect_state = AWS_PNCS_IN_PROGRESS;
    result = AWS_OP_SUCCESS;

done:

    if (result != AWS_OP_SUCCESS) {
        if (error_code == AWS_ERROR_SUCCESS) {
            error_code = AWS_ERROR_UNKNOWN;
        }
        negotiation_termination_callback(message, error_code, internal_proxy_user_data);
    } else {
        negotiation_http_request_forward_callback(message, internal_proxy_user_data);
    }

    aws_string_destroy(challenge_answer_token);
}

AWS_STATIC_STRING_FROM_LITERAL(s_ntlm_challenge_token_header, "Proxy-Authenticate");

static int s_ntlm_on_incoming_header_adaptive(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers) {

    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_negotiator = proxy_negotiator->impl;

    /*
     * only extract the challenge before we've started our own CONNECT attempt
     *
     * ToDo: we currently overwrite previous challenge tokens since it is unknown if multiple CONNECT requests
     * cause new challenges to be issued such that old challenges become invalid even if successfully computed
     */
    if (ntlm_negotiator->connect_state == AWS_PNCS_READY) {
        if (header_block == AWS_HTTP_HEADER_BLOCK_MAIN) {
            struct aws_byte_cursor proxy_authenticate_header_name =
                aws_byte_cursor_from_string(s_ntlm_challenge_token_header);
            for (size_t i = 0; i < num_headers; ++i) {
                struct aws_byte_cursor header_name_cursor = header_array[i].name;
                if (aws_byte_cursor_eq_ignore_case(&proxy_authenticate_header_name, &header_name_cursor)) {
                    aws_string_destroy(ntlm_negotiator->challenge_token);

                    struct aws_byte_cursor challenge_value_cursor = header_array[i].value;
                    ntlm_negotiator->challenge_token =
                        aws_string_new_from_cursor(ntlm_negotiator->allocator, &challenge_value_cursor);
                    break;
                }
            }
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_ntlm_on_connect_status(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_status_code status_code) {

    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_negotiator = proxy_negotiator->impl;

    if (ntlm_negotiator->connect_state == AWS_PNCS_IN_PROGRESS) {
        if (AWS_HTTP_STATUS_CODE_200_OK != status_code) {
            ntlm_negotiator->connect_state = AWS_PNCS_FAILURE;
        } else {
            ntlm_negotiator->connect_state = AWS_PNCS_SUCCESS;
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_ntlm_on_incoming_body(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    const struct aws_byte_cursor *data) {

    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_negotiator = proxy_negotiator->impl;
    (void)ntlm_negotiator;
    (void)data;

    return AWS_OP_SUCCESS;
}

static enum aws_http_proxy_negotiation_retry_directive s_ntlm_tunnel_get_retry_directive(
    struct aws_http_proxy_negotiator *proxy_negotiator) {
    (void)proxy_negotiator;

    return AWS_HPNRD_CURRENT_CONNECTION;
}

static struct aws_http_proxy_negotiator_tunnelling_vtable s_tunneling_ntlm_proxy_negotiator_tunneling_vtable = {
    .on_incoming_body_callback = s_ntlm_on_incoming_body,
    .on_incoming_headers_callback = s_ntlm_on_incoming_header_adaptive,
    .on_status_callback = s_ntlm_on_connect_status,
    .connect_request_transform = s_ntlm_tunnel_transform_connect,
    .get_retry_directive = s_ntlm_tunnel_get_retry_directive,
};

static void s_destroy_tunneling_ntlm_negotiator(struct aws_http_proxy_negotiator *proxy_negotiator) {
    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_negotiator = proxy_negotiator->impl;

    aws_string_destroy(ntlm_negotiator->challenge_token);
    aws_http_proxy_strategy_release(ntlm_negotiator->strategy);

    aws_mem_release(ntlm_negotiator->allocator, ntlm_negotiator);
}

static struct aws_http_proxy_negotiator *s_create_tunneling_ntlm_negotiator(
    struct aws_http_proxy_strategy *proxy_strategy,
    struct aws_allocator *allocator) {
    if (proxy_strategy == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_negotiator =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_negotiator_tunneling_ntlm));
    if (ntlm_negotiator == NULL) {
        return NULL;
    }

    ntlm_negotiator->allocator = allocator;
    ntlm_negotiator->negotiator_base.impl = ntlm_negotiator;
    aws_ref_count_init(
        &ntlm_negotiator->negotiator_base.ref_count,
        &ntlm_negotiator->negotiator_base,
        (aws_simple_completion_callback *)s_destroy_tunneling_ntlm_negotiator);

    ntlm_negotiator->negotiator_base.strategy_vtable.tunnelling_vtable =
        &s_tunneling_ntlm_proxy_negotiator_tunneling_vtable;

    ntlm_negotiator->strategy = aws_http_proxy_strategy_acquire(proxy_strategy);

    return &ntlm_negotiator->negotiator_base;
}

static struct aws_http_proxy_strategy_vtable s_tunneling_ntlm_strategy_vtable = {
    .create_negotiator = s_create_tunneling_ntlm_negotiator,
};

static void s_destroy_tunneling_ntlm_strategy(struct aws_http_proxy_strategy *proxy_strategy) {
    struct aws_http_proxy_strategy_tunneling_ntlm *ntlm_strategy = proxy_strategy->impl;

    aws_mem_release(ntlm_strategy->allocator, ntlm_strategy);
}

struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_ntlm(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_ntlm_options *config) {

    if (allocator == NULL || config == NULL || config->get_challenge_token == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_strategy_tunneling_ntlm *ntlm_strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_strategy_tunneling_ntlm));
    if (ntlm_strategy == NULL) {
        return NULL;
    }

    ntlm_strategy->strategy_base.impl = ntlm_strategy;
    ntlm_strategy->strategy_base.vtable = &s_tunneling_ntlm_strategy_vtable;
    ntlm_strategy->strategy_base.proxy_connection_type = AWS_HPCT_HTTP_TUNNEL;

    ntlm_strategy->allocator = allocator;

    aws_ref_count_init(
        &ntlm_strategy->strategy_base.ref_count,
        &ntlm_strategy->strategy_base,
        (aws_simple_completion_callback *)s_destroy_tunneling_ntlm_strategy);

    ntlm_strategy->get_challenge_token = config->get_challenge_token;
    ntlm_strategy->get_challenge_token_user_data = config->get_challenge_token_user_data;

    return &ntlm_strategy->strategy_base;
}
/******************************************************************************************************/

static void s_ntlm_credential_tunnel_transform_connect(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message,
    aws_http_proxy_negotiation_terminate_fn *negotiation_termination_callback,
    aws_http_proxy_negotiation_http_request_forward_fn *negotiation_http_request_forward_callback,
    void *internal_proxy_user_data) {

    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_credential_negotiator = proxy_negotiator->impl;
    struct aws_http_proxy_strategy_tunneling_ntlm *ntlm_credential_strategy =
        ntlm_credential_negotiator->strategy->impl;

    int result = AWS_OP_ERR;
    int error_code = AWS_ERROR_SUCCESS;
    struct aws_string *token = NULL;

    if (ntlm_credential_negotiator->connect_state == AWS_PNCS_FAILURE) {
        error_code = AWS_ERROR_HTTP_PROXY_CONNECT_FAILED;
        goto done;
    }

    if (ntlm_credential_negotiator->connect_state != AWS_PNCS_READY) {
        error_code = AWS_ERROR_INVALID_STATE;
        goto done;
    }

    ntlm_credential_negotiator->connect_state = AWS_PNCS_IN_PROGRESS;
    token = ntlm_credential_strategy->get_token(ntlm_credential_strategy->get_challenge_token_user_data, &error_code);

    if (token == NULL || error_code != AWS_ERROR_SUCCESS) {
        goto done;
    }

    /*transform the header with proxy authenticate:Negotiate and kerberos token*/
    if (s_add_ntlm_proxy_usertoken_authentication_header(
            ntlm_credential_negotiator->allocator, message, aws_byte_cursor_from_string(token))) {
        error_code = aws_last_error();
        goto done;
    }

    ntlm_credential_negotiator->connect_state = AWS_PNCS_IN_PROGRESS;
    result = AWS_OP_SUCCESS;

done:

    if (result != AWS_OP_SUCCESS) {
        if (error_code == AWS_ERROR_SUCCESS) {
            error_code = AWS_ERROR_UNKNOWN;
        }
        negotiation_termination_callback(message, error_code, internal_proxy_user_data);
    } else {
        negotiation_http_request_forward_callback(message, internal_proxy_user_data);
    }

    aws_string_destroy(token);
}

static struct aws_http_proxy_negotiator_tunnelling_vtable
    s_tunneling_ntlm_proxy_credential_negotiator_tunneling_vtable = {
        .on_incoming_body_callback = s_ntlm_on_incoming_body,
        .on_incoming_headers_callback = s_ntlm_on_incoming_header_adaptive,
        .on_status_callback = s_ntlm_on_connect_status,
        .connect_request_transform = s_ntlm_credential_tunnel_transform_connect,
};

static void s_destroy_tunneling_ntlm_credential_negotiator(struct aws_http_proxy_negotiator *proxy_negotiator) {
    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_credential_negotiator = proxy_negotiator->impl;

    aws_string_destroy(ntlm_credential_negotiator->challenge_token);
    aws_http_proxy_strategy_release(ntlm_credential_negotiator->strategy);

    aws_mem_release(ntlm_credential_negotiator->allocator, ntlm_credential_negotiator);
}

static struct aws_http_proxy_negotiator *s_create_tunneling_ntlm_credential_negotiator(
    struct aws_http_proxy_strategy *proxy_strategy,
    struct aws_allocator *allocator) {
    if (proxy_strategy == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_negotiator_tunneling_ntlm *ntlm_credential_negotiator =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_negotiator_tunneling_ntlm));
    if (ntlm_credential_negotiator == NULL) {
        return NULL;
    }

    ntlm_credential_negotiator->allocator = allocator;
    ntlm_credential_negotiator->negotiator_base.impl = ntlm_credential_negotiator;
    aws_ref_count_init(
        &ntlm_credential_negotiator->negotiator_base.ref_count,
        &ntlm_credential_negotiator->negotiator_base,
        (aws_simple_completion_callback *)s_destroy_tunneling_ntlm_credential_negotiator);

    ntlm_credential_negotiator->negotiator_base.strategy_vtable.tunnelling_vtable =
        &s_tunneling_ntlm_proxy_credential_negotiator_tunneling_vtable;

    ntlm_credential_negotiator->strategy = aws_http_proxy_strategy_acquire(proxy_strategy);

    return &ntlm_credential_negotiator->negotiator_base;
}

static struct aws_http_proxy_strategy_vtable s_tunneling_ntlm_credential_strategy_vtable = {
    .create_negotiator = s_create_tunneling_ntlm_credential_negotiator,
};

static void s_destroy_tunneling_ntlm_credential_strategy(struct aws_http_proxy_strategy *proxy_strategy) {
    struct aws_http_proxy_strategy_tunneling_ntlm *ntlm_credential_strategy = proxy_strategy->impl;

    aws_mem_release(ntlm_credential_strategy->allocator, ntlm_credential_strategy);
}

struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_ntlm_credential(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_ntlm_options *config) {

    if (allocator == NULL || config == NULL || config->get_token == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_strategy_tunneling_ntlm *ntlm_credential_strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_strategy_tunneling_ntlm));
    if (ntlm_credential_strategy == NULL) {
        return NULL;
    }

    ntlm_credential_strategy->strategy_base.impl = ntlm_credential_strategy;
    ntlm_credential_strategy->strategy_base.vtable = &s_tunneling_ntlm_credential_strategy_vtable;
    ntlm_credential_strategy->strategy_base.proxy_connection_type = AWS_HPCT_HTTP_TUNNEL;

    ntlm_credential_strategy->allocator = allocator;

    aws_ref_count_init(
        &ntlm_credential_strategy->strategy_base.ref_count,
        &ntlm_credential_strategy->strategy_base,
        (aws_simple_completion_callback *)s_destroy_tunneling_ntlm_credential_strategy);

    ntlm_credential_strategy->get_token = config->get_token;
    ntlm_credential_strategy->get_challenge_token_user_data = config->get_challenge_token_user_data;

    return &ntlm_credential_strategy->strategy_base;
}

/******************************************************************************************************************/

#define PROXY_STRATEGY_MAX_ADAPTIVE_STRATEGIES 4

struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_adaptive(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_adaptive_options *config) {

    if (allocator == NULL || config == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_strategy *strategies[PROXY_STRATEGY_MAX_ADAPTIVE_STRATEGIES];

    uint32_t strategy_count = 0;
    struct aws_http_proxy_strategy *identity_strategy = NULL;
    struct aws_http_proxy_strategy *kerberos_strategy = NULL;
    struct aws_http_proxy_strategy *ntlm_credential_strategy = NULL;
    struct aws_http_proxy_strategy *ntlm_strategy = NULL;
    struct aws_http_proxy_strategy *adaptive_sequence_strategy = NULL;

    identity_strategy = aws_http_proxy_strategy_new_tunneling_one_time_identity(allocator);
    if (identity_strategy == NULL) {
        goto done;
    }
    strategies[strategy_count++] = identity_strategy;

    if (config->kerberos_options != NULL) {
        kerberos_strategy = aws_http_proxy_strategy_new_tunneling_kerberos(allocator, config->kerberos_options);
        if (kerberos_strategy == NULL) {
            goto done;
        }

        strategies[strategy_count++] = kerberos_strategy;
    }

    if (config->ntlm_options != NULL) {
        ntlm_credential_strategy =
            aws_http_proxy_strategy_new_tunneling_ntlm_credential(allocator, config->ntlm_options);
        if (ntlm_credential_strategy == NULL) {
            goto done;
        }

        strategies[strategy_count++] = ntlm_credential_strategy;

        ntlm_strategy = aws_http_proxy_strategy_new_tunneling_ntlm(allocator, config->ntlm_options);
        if (ntlm_strategy == NULL) {
            goto done;
        }

        strategies[strategy_count++] = ntlm_strategy;
    }

    AWS_FATAL_ASSERT(strategy_count <= PROXY_STRATEGY_MAX_ADAPTIVE_STRATEGIES);

    struct aws_http_proxy_strategy_tunneling_sequence_options sequence_config = {
        .strategies = strategies,
        .strategy_count = strategy_count,
    };

    adaptive_sequence_strategy = aws_http_proxy_strategy_new_tunneling_sequence(allocator, &sequence_config);
    if (adaptive_sequence_strategy == NULL) {
        goto done;
    }

done:

    aws_http_proxy_strategy_release(identity_strategy);
    aws_http_proxy_strategy_release(kerberos_strategy);
    aws_http_proxy_strategy_release(ntlm_credential_strategy);
    aws_http_proxy_strategy_release(ntlm_strategy);

    return adaptive_sequence_strategy;
}

/******************************************************************************************************************/

struct aws_http_proxy_strategy_tunneling_sequence {
    struct aws_allocator *allocator;

    struct aws_array_list strategies;

    struct aws_http_proxy_strategy strategy_base;
};

struct aws_http_proxy_negotiator_tunneling_sequence {
    struct aws_allocator *allocator;

    struct aws_array_list negotiators;
    size_t current_negotiator_transform_index;
    void *original_internal_proxy_user_data;
    aws_http_proxy_negotiation_terminate_fn *original_negotiation_termination_callback;
    aws_http_proxy_negotiation_http_request_forward_fn *original_negotiation_http_request_forward_callback;

    struct aws_http_proxy_negotiator negotiator_base;
};

static void s_sequence_tunnel_iteration_termination_callback(
    struct aws_http_message *message,
    int error_code,
    void *user_data) {

    struct aws_http_proxy_negotiator *proxy_negotiator = user_data;
    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;

    AWS_LOGF_WARN(
        AWS_LS_HTTP_PROXY_NEGOTIATION,
        "(id=%p) Proxy negotiation step failed with error %d",
        (void *)proxy_negotiator,
        error_code);

    int connection_error_code = AWS_ERROR_HTTP_PROXY_CONNECT_FAILED_RETRYABLE;
    if (sequence_negotiator->current_negotiator_transform_index >=
        aws_array_list_length(&sequence_negotiator->negotiators)) {
        connection_error_code = AWS_ERROR_HTTP_PROXY_CONNECT_FAILED;
    }

    sequence_negotiator->original_negotiation_termination_callback(
        message, connection_error_code, sequence_negotiator->original_internal_proxy_user_data);
}

static void s_sequence_tunnel_iteration_forward_callback(struct aws_http_message *message, void *user_data) {
    struct aws_http_proxy_negotiator *proxy_negotiator = user_data;
    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;

    sequence_negotiator->original_negotiation_http_request_forward_callback(
        message, sequence_negotiator->original_internal_proxy_user_data);
}

static void s_sequence_tunnel_try_next_negotiator(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message) {
    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;

    size_t negotiator_count = aws_array_list_length(&sequence_negotiator->negotiators);
    if (sequence_negotiator->current_negotiator_transform_index >= negotiator_count) {
        goto on_error;
    }

    struct aws_http_proxy_negotiator *current_negotiator = NULL;
    if (aws_array_list_get_at(
            &sequence_negotiator->negotiators,
            &current_negotiator,
            sequence_negotiator->current_negotiator_transform_index++)) {
        goto on_error;
    }

    current_negotiator->strategy_vtable.tunnelling_vtable->connect_request_transform(
        current_negotiator,
        message,
        s_sequence_tunnel_iteration_termination_callback,
        s_sequence_tunnel_iteration_forward_callback,
        proxy_negotiator);

    return;

on_error:

    sequence_negotiator->original_negotiation_termination_callback(
        message, AWS_ERROR_HTTP_PROXY_CONNECT_FAILED, sequence_negotiator->original_internal_proxy_user_data);
}

static void s_sequence_tunnel_transform_connect(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    struct aws_http_message *message,
    aws_http_proxy_negotiation_terminate_fn *negotiation_termination_callback,
    aws_http_proxy_negotiation_http_request_forward_fn *negotiation_http_request_forward_callback,
    void *internal_proxy_user_data) {

    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;

    sequence_negotiator->original_internal_proxy_user_data = internal_proxy_user_data;
    sequence_negotiator->original_negotiation_termination_callback = negotiation_termination_callback;
    sequence_negotiator->original_negotiation_http_request_forward_callback = negotiation_http_request_forward_callback;

    s_sequence_tunnel_try_next_negotiator(proxy_negotiator, message);
}

static int s_sequence_on_incoming_headers(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers) {

    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;
    size_t negotiator_count = aws_array_list_length(&sequence_negotiator->negotiators);
    for (size_t i = 0; i < negotiator_count; ++i) {
        struct aws_http_proxy_negotiator *negotiator = NULL;
        if (aws_array_list_get_at(&sequence_negotiator->negotiators, &negotiator, i)) {
            continue;
        }

        aws_http_proxy_negotiation_connect_on_incoming_headers_fn *on_incoming_headers =
            negotiator->strategy_vtable.tunnelling_vtable->on_incoming_headers_callback;
        if (on_incoming_headers != NULL) {
            (*on_incoming_headers)(negotiator, header_block, header_array, num_headers);
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_sequence_on_connect_status(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    enum aws_http_status_code status_code) {

    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;
    size_t negotiator_count = aws_array_list_length(&sequence_negotiator->negotiators);
    for (size_t i = 0; i < negotiator_count; ++i) {
        struct aws_http_proxy_negotiator *negotiator = NULL;
        if (aws_array_list_get_at(&sequence_negotiator->negotiators, &negotiator, i)) {
            continue;
        }

        aws_http_proxy_negotiator_connect_status_fn *on_status =
            negotiator->strategy_vtable.tunnelling_vtable->on_status_callback;
        if (on_status != NULL) {
            (*on_status)(negotiator, status_code);
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_sequence_on_incoming_body(
    struct aws_http_proxy_negotiator *proxy_negotiator,
    const struct aws_byte_cursor *data) {

    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;
    size_t negotiator_count = aws_array_list_length(&sequence_negotiator->negotiators);
    for (size_t i = 0; i < negotiator_count; ++i) {
        struct aws_http_proxy_negotiator *negotiator = NULL;
        if (aws_array_list_get_at(&sequence_negotiator->negotiators, &negotiator, i)) {
            continue;
        }

        aws_http_proxy_negotiator_connect_on_incoming_body_fn *on_incoming_body =
            negotiator->strategy_vtable.tunnelling_vtable->on_incoming_body_callback;
        if (on_incoming_body != NULL) {
            (*on_incoming_body)(negotiator, data);
        }
    }

    return AWS_OP_SUCCESS;
}

static enum aws_http_proxy_negotiation_retry_directive s_sequence_get_retry_directive(
    struct aws_http_proxy_negotiator *proxy_negotiator) {
    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;

    if (sequence_negotiator->current_negotiator_transform_index <
        aws_array_list_length(&sequence_negotiator->negotiators)) {
        struct aws_http_proxy_negotiator *next_negotiator = NULL;
        aws_array_list_get_at(
            &sequence_negotiator->negotiators,
            &next_negotiator,
            sequence_negotiator->current_negotiator_transform_index);

        enum aws_http_proxy_negotiation_retry_directive next_negotiator_directive =
            aws_http_proxy_negotiator_get_retry_directive(next_negotiator);
        if (next_negotiator_directive == AWS_HPNRD_CURRENT_CONNECTION) {
            return AWS_HPNRD_CURRENT_CONNECTION;
        } else {
            return AWS_HPNRD_NEW_CONNECTION;
        }
    }

    return AWS_HPNRD_STOP;
}

static struct aws_http_proxy_negotiator_tunnelling_vtable s_tunneling_sequence_proxy_negotiator_tunneling_vtable = {
    .on_incoming_body_callback = s_sequence_on_incoming_body,
    .on_incoming_headers_callback = s_sequence_on_incoming_headers,
    .on_status_callback = s_sequence_on_connect_status,
    .connect_request_transform = s_sequence_tunnel_transform_connect,
    .get_retry_directive = s_sequence_get_retry_directive,
};

static void s_destroy_tunneling_sequence_negotiator(struct aws_http_proxy_negotiator *proxy_negotiator) {
    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator = proxy_negotiator->impl;

    size_t negotiator_count = aws_array_list_length(&sequence_negotiator->negotiators);
    for (size_t i = 0; i < negotiator_count; ++i) {
        struct aws_http_proxy_negotiator *negotiator = NULL;
        if (aws_array_list_get_at(&sequence_negotiator->negotiators, &negotiator, i)) {
            continue;
        }

        aws_http_proxy_negotiator_release(negotiator);
    }

    aws_array_list_clean_up(&sequence_negotiator->negotiators);

    aws_mem_release(sequence_negotiator->allocator, sequence_negotiator);
}

static struct aws_http_proxy_negotiator *s_create_tunneling_sequence_negotiator(
    struct aws_http_proxy_strategy *proxy_strategy,
    struct aws_allocator *allocator) {
    if (proxy_strategy == NULL || allocator == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_negotiator_tunneling_sequence *sequence_negotiator =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_negotiator_tunneling_sequence));
    if (sequence_negotiator == NULL) {
        return NULL;
    }

    sequence_negotiator->allocator = allocator;
    sequence_negotiator->negotiator_base.impl = sequence_negotiator;
    aws_ref_count_init(
        &sequence_negotiator->negotiator_base.ref_count,
        &sequence_negotiator->negotiator_base,
        (aws_simple_completion_callback *)s_destroy_tunneling_sequence_negotiator);

    sequence_negotiator->negotiator_base.strategy_vtable.tunnelling_vtable =
        &s_tunneling_sequence_proxy_negotiator_tunneling_vtable;

    struct aws_http_proxy_strategy_tunneling_sequence *sequence_strategy = proxy_strategy->impl;
    size_t strategy_count = aws_array_list_length(&sequence_strategy->strategies);

    if (aws_array_list_init_dynamic(
            &sequence_negotiator->negotiators, allocator, strategy_count, sizeof(struct aws_http_proxy_negotiator *))) {
        goto on_error;
    }

    for (size_t i = 0; i < strategy_count; ++i) {
        struct aws_http_proxy_strategy *strategy = NULL;
        if (aws_array_list_get_at(&sequence_strategy->strategies, &strategy, i)) {
            goto on_error;
        }

        struct aws_http_proxy_negotiator *negotiator = aws_http_proxy_strategy_create_negotiator(strategy, allocator);
        if (negotiator == NULL) {
            goto on_error;
        }

        if (aws_array_list_push_back(&sequence_negotiator->negotiators, &negotiator)) {
            aws_http_proxy_negotiator_release(negotiator);
            goto on_error;
        }
    }

    return &sequence_negotiator->negotiator_base;

on_error:

    aws_http_proxy_negotiator_release(&sequence_negotiator->negotiator_base);

    return NULL;
}

static struct aws_http_proxy_strategy_vtable s_tunneling_sequence_strategy_vtable = {
    .create_negotiator = s_create_tunneling_sequence_negotiator,
};

static void s_destroy_tunneling_sequence_strategy(struct aws_http_proxy_strategy *proxy_strategy) {
    struct aws_http_proxy_strategy_tunneling_sequence *sequence_strategy = proxy_strategy->impl;

    size_t strategy_count = aws_array_list_length(&sequence_strategy->strategies);
    for (size_t i = 0; i < strategy_count; ++i) {
        struct aws_http_proxy_strategy *strategy = NULL;
        if (aws_array_list_get_at(&sequence_strategy->strategies, &strategy, i)) {
            continue;
        }

        aws_http_proxy_strategy_release(strategy);
    }

    aws_array_list_clean_up(&sequence_strategy->strategies);

    aws_mem_release(sequence_strategy->allocator, sequence_strategy);
}

struct aws_http_proxy_strategy *aws_http_proxy_strategy_new_tunneling_sequence(
    struct aws_allocator *allocator,
    struct aws_http_proxy_strategy_tunneling_sequence_options *config) {

    if (allocator == NULL || config == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_http_proxy_strategy_tunneling_sequence *sequence_strategy =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_strategy_tunneling_sequence));
    if (sequence_strategy == NULL) {
        return NULL;
    }

    sequence_strategy->strategy_base.impl = sequence_strategy;
    sequence_strategy->strategy_base.vtable = &s_tunneling_sequence_strategy_vtable;
    sequence_strategy->strategy_base.proxy_connection_type = AWS_HPCT_HTTP_TUNNEL;
    sequence_strategy->allocator = allocator;

    aws_ref_count_init(
        &sequence_strategy->strategy_base.ref_count,
        &sequence_strategy->strategy_base,
        (aws_simple_completion_callback *)s_destroy_tunneling_sequence_strategy);

    if (aws_array_list_init_dynamic(
            &sequence_strategy->strategies,
            allocator,
            config->strategy_count,
            sizeof(struct aws_http_proxy_strategy *))) {
        goto on_error;
    }

    for (size_t i = 0; i < config->strategy_count; ++i) {
        struct aws_http_proxy_strategy *strategy = config->strategies[i];

        if (aws_array_list_push_back(&sequence_strategy->strategies, &strategy)) {
            goto on_error;
        }

        aws_http_proxy_strategy_acquire(strategy);
    }

    return &sequence_strategy->strategy_base;

on_error:

    aws_http_proxy_strategy_release(&sequence_strategy->strategy_base);

    return NULL;
}

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif /* _MSC_VER */
