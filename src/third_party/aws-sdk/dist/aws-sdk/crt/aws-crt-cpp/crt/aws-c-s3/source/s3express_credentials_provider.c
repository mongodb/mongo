/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include "aws/s3/s3express_credentials_provider.h"
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3express_credentials_provider_impl.h"
#include <aws/auth/credentials.h>
#include <aws/s3/private/s3_util.h>
#include <aws/s3/s3_client.h>

#include <aws/common/clock.h>
#include <aws/common/lru_cache.h>
#include <aws/common/uri.h>
#include <aws/common/xml_parser.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>

#include <aws/cal/hash.h>

#include <inttypes.h>

static struct aws_byte_cursor s_create_session_path_query = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/?session=");
static const size_t s_default_cache_capacity = 100;

/* Those number are from C++ SDK impl */
static const uint64_t s_expired_threshold_secs = 5;
static const uint64_t s_about_to_expire_threshold_secs = 60;
static const uint64_t s_background_refresh_interval_secs = 60;

struct aws_query_callback_node {
    struct aws_linked_list_node node;
    aws_on_get_credentials_callback_fn *get_cred_callback;
    void *get_cred_user_data;
};

struct aws_s3express_session_creator {
    struct aws_allocator *allocator;

    /* The hash key for the table storing creator and session. */
    struct aws_string *hash_key;

    struct aws_s3express_credentials_provider *provider;
    struct aws_byte_buf response_buf;

    /* The region and host of the session we are creating */
    struct aws_string *region;
    struct aws_string *host;

    struct {
        /* Protected by the impl lock */

        /* If creating a new session, this is NULL.
         * If refreshing an existing session, this points to it. */
        struct aws_s3express_session *session;
        /* Node of `struct aws_query_callback_node*` */
        struct aws_linked_list query_queue;
        struct aws_s3_meta_request *meta_request;
    } synced_data;
};

static struct aws_s3express_session *s_aws_s3express_session_new(
    struct aws_s3express_credentials_provider *provider,
    const struct aws_string *hash_key,
    const struct aws_string *region,
    const struct aws_string *host,
    struct aws_credentials *credentials) {

    struct aws_s3express_session *session =
        aws_mem_calloc(provider->allocator, 1, sizeof(struct aws_s3express_session));
    session->allocator = provider->allocator;
    session->impl = provider->impl;
    session->hash_key = aws_string_new_from_string(provider->allocator, hash_key);
    session->host = aws_string_new_from_string(provider->allocator, host);
    if (region) {
        session->region = aws_string_new_from_string(provider->allocator, region);
    }
    session->s3express_credentials = credentials;
    aws_credentials_acquire(credentials);
    return session;
}

static void s_aws_s3express_session_destroy(struct aws_s3express_session *session) {
    if (!session) {
        return;
    }
    if (session->creator) {
        /* The session is always protected by the lock, we can safely touch the synced data here */
        /* Unset the session, but keep the creator going */
        session->creator->synced_data.session = NULL;
    }
    aws_string_destroy(session->hash_key);
    aws_string_destroy(session->region);
    aws_string_destroy(session->host);
    aws_credentials_release(session->s3express_credentials);
    aws_mem_release(session->allocator, session);
}

static bool s_s3express_session_is_valid(struct aws_s3express_session *session, uint64_t now_seconds) {
    AWS_ASSERT(session->s3express_credentials);
    if (session->impl->mock_test.s3express_session_is_valid_override) {
        /* Mock override for testing. */
        return session->impl->mock_test.s3express_session_is_valid_override(session, now_seconds);
    }
    uint64_t expire_secs = aws_credentials_get_expiration_timepoint_seconds(session->s3express_credentials);
    uint64_t threshold_secs = 0;
    int overflow = aws_add_u64_checked(now_seconds, s_expired_threshold_secs, &threshold_secs);
    AWS_ASSERT(!overflow);
    (void)overflow;
    /* If it's too close to be expired, we consider the session is invalid */
    return threshold_secs < expire_secs;
}

static bool s_s3express_session_about_to_expire(struct aws_s3express_session *session, uint64_t now_seconds) {
    AWS_ASSERT(session->s3express_credentials);
    if (session->impl->mock_test.s3express_session_about_to_expire_override) {
        /* Mock override for testing. */
        return session->impl->mock_test.s3express_session_about_to_expire_override(session, now_seconds);
    }
    uint64_t expire_secs = aws_credentials_get_expiration_timepoint_seconds(session->s3express_credentials);
    uint64_t threshold_secs = 0;
    int overflow = aws_add_u64_checked(now_seconds, s_about_to_expire_threshold_secs, &threshold_secs);
    AWS_ASSERT(!overflow);
    (void)overflow;
    return threshold_secs >= expire_secs;
}

static struct aws_s3express_session_creator *s_aws_s3express_session_creator_destroy(
    struct aws_s3express_session_creator *session_creator);

static void s_credentials_provider_s3express_impl_lock_synced_data(
    struct aws_s3express_credentials_provider_impl *impl) {
    int err = aws_mutex_lock(&impl->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

static void s_credentials_provider_s3express_impl_unlock_synced_data(
    struct aws_s3express_credentials_provider_impl *impl) {
    int err = aws_mutex_unlock(&impl->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

static int s_on_incoming_body_fn(
    struct aws_s3_meta_request *meta_request,
    const struct aws_byte_cursor *body,
    uint64_t range_start,
    void *user_data) {
    (void)meta_request;
    (void)range_start;

    struct aws_s3express_session_creator *session_creator = user_data;
    return aws_byte_buf_append_dynamic(&session_creator->response_buf, body);
}

/* parse credentials of form
<?xml version="1.0" encoding="UTF-8"?>
<CreateSessionResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Credentials>
    <SessionToken>sessionToken</SessionToken>
    <SecretAccessKey>secretKey</SecretAccessKey>
    <AccessKeyId>accessKeyId</AccessKeyId>
    <Expiration>2023-06-26T17:33:30Z</Expiration>
  </Credentials>
</CreateSessionResult>
 */

struct aws_s3express_xml_parser_user_data {
    struct aws_allocator *allocator;
    struct aws_string *access_key_id;
    struct aws_string *secret_access_key;
    struct aws_string *session_token;
    void *log_id;
    uint64_t expire_timestamp_secs;
};

static int s_s3express_xml_traversing_credentials(struct aws_xml_node *node, void *user_data) {

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);

    struct aws_s3express_xml_parser_user_data *parser_ud = user_data;
    struct aws_byte_cursor credential_data;
    AWS_ZERO_STRUCT(credential_data);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "SessionToken")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        parser_ud->session_token =
            aws_string_new_from_array(parser_ud->allocator, credential_data.ptr, credential_data.len);
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "SecretAccessKey")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        parser_ud->secret_access_key =
            aws_string_new_from_array(parser_ud->allocator, credential_data.ptr, credential_data.len);
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "AccessKeyId")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        parser_ud->access_key_id =
            aws_string_new_from_array(parser_ud->allocator, credential_data.ptr, credential_data.len);
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Expiration")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        AWS_LOGF_TRACE(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Read Expiration " PRInSTR "",
            (void *)parser_ud->log_id,
            AWS_BYTE_CURSOR_PRI(credential_data));
        struct aws_date_time dt;
        if (aws_date_time_init_from_str_cursor(&dt, &credential_data, AWS_DATE_FORMAT_AUTO_DETECT)) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): Failed to parse Expiration " PRInSTR "",
                (void *)parser_ud->log_id,
                AWS_BYTE_CURSOR_PRI(credential_data));
            return AWS_OP_ERR;
        }
        parser_ud->expire_timestamp_secs = (uint64_t)aws_date_time_as_epoch_secs(&dt);
    }

    return AWS_OP_SUCCESS;
}

static int s_s3express_xml_traversing_CreateSessionResult_children(struct aws_xml_node *node, void *user_data) {

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Credentials")) {
        return aws_xml_node_traverse(node, s_s3express_xml_traversing_credentials, user_data);
    }

    return AWS_OP_SUCCESS;
}

static int s_s3express_xml_traversing_root(struct aws_xml_node *node, void *user_data) {

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "CreateSessionResult")) {
        return aws_xml_node_traverse(node, s_s3express_xml_traversing_CreateSessionResult_children, user_data);
    }

    return AWS_OP_SUCCESS;
}

static struct aws_credentials *s_parse_s3express_xml(
    struct aws_allocator *alloc,
    struct aws_byte_cursor xml,
    void *logging_id) {

    struct aws_credentials *credentials = NULL;

    struct aws_s3express_xml_parser_user_data user_data = {
        .allocator = alloc,
        .log_id = logging_id,
    };
    struct aws_xml_parser_options options = {
        .doc = xml,
        .on_root_encountered = s_s3express_xml_traversing_root,
        .user_data = &user_data,
    };
    if (aws_xml_parse(alloc, &options)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): credentials parsing failed with error %s",
            logging_id,
            aws_error_debug_str(aws_last_error()));
        goto done;
    }
    if (user_data.access_key_id && user_data.secret_access_key && user_data.session_token &&
        user_data.expire_timestamp_secs) {

        credentials = aws_credentials_new_from_string(
            alloc,
            user_data.access_key_id,
            user_data.secret_access_key,
            user_data.session_token,
            user_data.expire_timestamp_secs);
    }

done:
    /* Clean up resource */
    aws_string_destroy(user_data.access_key_id);
    aws_string_destroy(user_data.secret_access_key);
    aws_string_destroy(user_data.session_token);

    return credentials;
}

/* called upon completion of meta request */
static void s_on_request_finished(
    struct aws_s3_meta_request *meta_request,
    const struct aws_s3_meta_request_result *meta_request_result,
    void *user_data) {
    (void)meta_request;
    struct aws_s3express_session_creator *session_creator = user_data;
    struct aws_s3express_credentials_provider_impl *impl = session_creator->provider->impl;
    if (impl->mock_test.meta_request_finished_overhead) {
        impl->mock_test.meta_request_finished_overhead(meta_request, meta_request_result, user_data);
    }

    struct aws_linked_list pending_callbacks;
    aws_linked_list_init(&pending_callbacks);

    struct aws_credentials *credentials = NULL;
    int error_code = meta_request_result->error_code;

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): CreateSession call completed with http status: %d and error code %s",
        (void *)session_creator->provider,
        meta_request_result->response_status,
        aws_error_debug_str(error_code));

    if (error_code && meta_request_result->error_response_body && meta_request_result->error_response_body->len > 0) {
        /* The Create Session failed with an error response from S3, provide a specific error code for user. */
        error_code = AWS_ERROR_S3EXPRESS_CREATE_SESSION_FAILED;
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): CreateSession call failed with http status: %d, and error response body is: %.*s",
            (void *)session_creator->provider,
            meta_request_result->response_status,
            (int)meta_request_result->error_response_body->len,
            meta_request_result->error_response_body->buffer);
    }

    if (error_code == AWS_ERROR_SUCCESS) {
        credentials = s_parse_s3express_xml(
            session_creator->allocator, aws_byte_cursor_from_buf(&session_creator->response_buf), session_creator);

        if (!credentials) {
            error_code = AWS_AUTH_PROVIDER_PARSER_UNEXPECTED_RESPONSE;
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): failed to read credentials from document, treating as an error.",
                (void *)session_creator->provider);
        }
    }
    { /* BEGIN CRITICAL SECTION */
        s_credentials_provider_s3express_impl_lock_synced_data(impl);
        aws_linked_list_swap_contents(&session_creator->synced_data.query_queue, &pending_callbacks);
        aws_hash_table_remove(&impl->synced_data.session_creator_table, session_creator->hash_key, NULL, NULL);
        struct aws_s3express_session *session = session_creator->synced_data.session;
        if (session) {
            session->creator = NULL;
            if (error_code == AWS_ERROR_SUCCESS) {
                /* The session already existed, just update the credentials for the session */
                aws_credentials_release(session->s3express_credentials);
                session->s3express_credentials = credentials;
                aws_credentials_acquire(credentials);
            } else {
                /* The session failed to be created, remove the session from the cache. */
                aws_cache_remove(impl->synced_data.cache, session->hash_key);
            }
        } else if (error_code == AWS_ERROR_SUCCESS) {
            /* Create a new session when we get valid credentials and put it into cache */
            session = s_aws_s3express_session_new(
                session_creator->provider,
                session_creator->hash_key,
                session_creator->region,
                session_creator->host,
                credentials);
            aws_cache_put(impl->synced_data.cache, session->hash_key, session);
        }

        s_credentials_provider_s3express_impl_unlock_synced_data(impl);
    } /* END CRITICAL SECTION */

    /* Invoked all callbacks */
    while (!aws_linked_list_empty(&pending_callbacks)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&pending_callbacks);
        struct aws_query_callback_node *callback_node = AWS_CONTAINER_OF(node, struct aws_query_callback_node, node);
        callback_node->get_cred_callback(credentials, error_code, callback_node->get_cred_user_data);
        aws_mem_release(session_creator->allocator, callback_node);
    }
    aws_credentials_release(credentials);
    s_aws_s3express_session_creator_destroy(session_creator);
}

static struct aws_http_message *s_create_session_request_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor host_value) {
    struct aws_http_message *request = aws_http_message_new_request(allocator);

    struct aws_http_header host_header = {
        .name = g_host_header_name,
        .value = host_value,
    };

    if (aws_http_message_add_header(request, host_header)) {
        goto error;
    }

    struct aws_http_header user_agent_header = {
        .name = g_user_agent_header_name,
        .value = aws_byte_cursor_from_c_str("aws-sdk-crt/s3express-credentials-provider"),
    };
    if (aws_http_message_add_header(request, user_agent_header)) {
        goto error;
    }

    if (aws_http_message_set_request_method(request, aws_http_method_get)) {
        goto error;
    }

    if (aws_http_message_set_request_path(request, s_create_session_path_query)) {
        goto error;
    }
    return request;
error:
    return aws_http_message_release(request);
}

/* Clean up resources that only related to one create session call */
static struct aws_s3express_session_creator *s_aws_s3express_session_creator_destroy(
    struct aws_s3express_session_creator *session_creator) {
    if (session_creator == NULL) {
        return NULL;
    }
    AWS_FATAL_ASSERT(aws_linked_list_empty(&session_creator->synced_data.query_queue));
    struct aws_s3express_credentials_provider_impl *impl = session_creator->provider->impl;
    aws_s3_meta_request_release(session_creator->synced_data.meta_request);
    aws_ref_count_release(&impl->internal_ref);

    aws_string_destroy(session_creator->hash_key);
    aws_string_destroy(session_creator->region);
    aws_string_destroy(session_creator->host);

    aws_byte_buf_clean_up(&session_creator->response_buf);
    aws_mem_release(session_creator->allocator, session_creator);
    return NULL;
}

/**
 * Encode the hash key to be [host_value][hash_of_credentials]
 * hash_of_credentials is the sha256 of [access_key][secret_access_key]
 **/
struct aws_string *aws_encode_s3express_hash_key_new(
    struct aws_allocator *allocator,
    const struct aws_credentials *original_credentials,
    struct aws_byte_cursor host_value) {

    struct aws_byte_buf combine_key_buf;

    /* 1. Combine access_key and secret_access_key into one buffer */
    struct aws_byte_cursor access_key = aws_credentials_get_access_key_id(original_credentials);
    struct aws_byte_cursor secret_access_key = aws_credentials_get_secret_access_key(original_credentials);
    aws_byte_buf_init(&combine_key_buf, allocator, access_key.len + secret_access_key.len);
    aws_byte_buf_write_from_whole_cursor(&combine_key_buf, access_key);
    aws_byte_buf_write_from_whole_cursor(&combine_key_buf, secret_access_key);

    /* 2. Get sha256 digest from the combined key */
    struct aws_byte_cursor combine_key = aws_byte_cursor_from_buf(&combine_key_buf);
    struct aws_byte_buf digest_buf;
    aws_byte_buf_init(&digest_buf, allocator, AWS_SHA256_LEN);
    aws_sha256_compute(allocator, &combine_key, &digest_buf, 0);

    /* 3. Encode the result to be [host_value][hash_of_credentials] */
    struct aws_byte_buf result_buffer;
    aws_byte_buf_init(&result_buffer, allocator, host_value.len + digest_buf.len);
    aws_byte_buf_write_from_whole_cursor(&result_buffer, host_value);
    aws_byte_buf_write_from_whole_buffer(&result_buffer, digest_buf);
    struct aws_string *result = aws_string_new_from_buf(allocator, &result_buffer);

    /* Clean up */
    aws_byte_buf_clean_up(&result_buffer);
    aws_byte_buf_clean_up(&combine_key_buf);
    aws_byte_buf_clean_up(&digest_buf);

    return result;
}

static struct aws_s3express_session_creator *s_session_creator_new(
    struct aws_s3express_credentials_provider *provider,
    const struct aws_credentials *original_credentials,
    const struct aws_credentials_properties_s3express *s3express_properties) {

    struct aws_s3express_credentials_provider_impl *impl = provider->impl;
    struct aws_http_message *request = s_create_session_request_new(provider->allocator, s3express_properties->host);
    if (!request) {
        return NULL;
    }
    if (impl->mock_test.endpoint_override) {
        /* NOTE: ONLY FOR TESTS. Erase the host header for endpoint override. */
        aws_http_headers_erase(aws_http_message_get_headers(request), g_host_header_name);
    }

    struct aws_s3express_session_creator *session_creator =
        aws_mem_calloc(provider->allocator, 1, sizeof(struct aws_s3express_session_creator));
    session_creator->allocator = provider->allocator;
    session_creator->provider = provider;
    session_creator->host = aws_string_new_from_cursor(session_creator->allocator, &s3express_properties->host);
    session_creator->region = aws_string_new_from_cursor(session_creator->allocator, &s3express_properties->region);

    struct aws_signing_config_aws s3express_signing_config = {
        .credentials = original_credentials,
        .service = g_s3express_service_name,
        .region = s3express_properties->region,
    };

    aws_byte_buf_init(&session_creator->response_buf, provider->allocator, 512);
    struct aws_s3_meta_request_options options = {
        .message = request,
        .type = AWS_S3_META_REQUEST_TYPE_DEFAULT,
        .body_callback = s_on_incoming_body_fn,
        .finish_callback = s_on_request_finished,
        .signing_config = &s3express_signing_config,
        /* Override endpoint only for tests. */
        .endpoint = impl->mock_test.endpoint_override ? impl->mock_test.endpoint_override : NULL,
        .user_data = session_creator,
        .operation_name = aws_byte_cursor_from_c_str("CreateSession"),
    };
    session_creator->synced_data.meta_request = aws_s3_client_make_meta_request(impl->client, &options);
    AWS_FATAL_ASSERT(session_creator->synced_data.meta_request);
    aws_http_message_release(request);
    aws_ref_count_acquire(&impl->internal_ref);
    aws_linked_list_init(&session_creator->synced_data.query_queue);

    return session_creator;
}

static int s_s3express_get_creds(
    struct aws_s3express_credentials_provider *provider,
    const struct aws_credentials *original_credentials,
    const struct aws_credentials_properties_s3express *s3express_properties,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {
    if (s3express_properties->host.len == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): The host property is empty to get credentials from S3 Express",
            (void *)provider);

        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_s3express_credentials_provider_impl *impl = provider->impl;

    struct aws_hash_element *session_creator_hash_element = NULL;
    int was_created = 0;
    struct aws_credentials *s3express_credentials = NULL;
    struct aws_byte_cursor access_key;
    AWS_ZERO_STRUCT(access_key);
    if (original_credentials) {
        access_key = aws_credentials_get_access_key_id(original_credentials);
    }

    uint64_t current_stamp = UINT64_MAX;
    aws_sys_clock_get_ticks(&current_stamp);
    struct aws_string *hash_key =
        aws_encode_s3express_hash_key_new(provider->allocator, original_credentials, s3express_properties->host);
    uint64_t now_seconds = aws_timestamp_convert(current_stamp, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, NULL);

    s_credentials_provider_s3express_impl_lock_synced_data(impl);
    /* Used after free is a crime */
    AWS_FATAL_ASSERT(!impl->synced_data.destroying);
    /* Step 1: Check cache. */
    struct aws_s3express_session *session = NULL;
    int ret_code = aws_cache_find(impl->synced_data.cache, hash_key, (void **)&session);
    AWS_ASSERT(ret_code == AWS_OP_SUCCESS);
    if (session) {
        /* We found a session */
        session->inactive = false;
        AWS_ASSERT(session->s3express_credentials != NULL);
        if (s_s3express_session_is_valid(session, now_seconds)) {
            s3express_credentials = session->s3express_credentials;
            /* Make sure the creds are valid until the callback invokes */
            aws_credentials_acquire(s3express_credentials);
            aws_string_destroy(hash_key);
            goto unlock;
        } else {
            /* Remove the session from cache and fall to try to creating the session */
            aws_cache_remove(impl->synced_data.cache, hash_key);
        }
    }

    /* Step 2: Check the creator map */
    ret_code = aws_hash_table_create(
        &impl->synced_data.session_creator_table, hash_key, &session_creator_hash_element, &was_created);
    AWS_ASSERT(ret_code == AWS_OP_SUCCESS);
    (void)ret_code;

    /* Step 3: Create session if needed */
    if (was_created) {
        /* A new session creator needed */
        struct aws_s3express_session_creator *new_session_creator =
            s_session_creator_new(provider, original_credentials, s3express_properties);
        /* If we failed to create session creator, it's probably OOM or impl error we don't want to handle */
        AWS_FATAL_ASSERT(new_session_creator);
        new_session_creator->hash_key = hash_key;
        session_creator_hash_element->value = new_session_creator;
    } else {
        aws_string_destroy(hash_key);
    }

    if (s3express_credentials == NULL) {
        /* Queue the callback if we don't have a creds to return now. */
        struct aws_s3express_session_creator *session_creator = session_creator_hash_element->value;
        struct aws_query_callback_node *callback_node =
            aws_mem_acquire(provider->allocator, sizeof(struct aws_query_callback_node));
        callback_node->get_cred_callback = callback;
        callback_node->get_cred_user_data = user_data;
        aws_linked_list_push_back(&session_creator->synced_data.query_queue, &callback_node->node);
    }
unlock:
    s_credentials_provider_s3express_impl_unlock_synced_data(impl);
    if (s3express_credentials) {
        uint64_t expire_secs = aws_credentials_get_expiration_timepoint_seconds(s3express_credentials);
        AWS_LOGF_TRACE(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Found credentials from cache. Timestamp to expire is %" PRIu64 ", while now is %" PRIu64 ".",
            (void *)provider,
            expire_secs,
            now_seconds);
        /* TODO: invoke callback asynced? */
        callback(s3express_credentials, AWS_ERROR_SUCCESS, user_data);
        aws_credentials_release(s3express_credentials);
        return AWS_OP_SUCCESS;
    }
    return AWS_OP_SUCCESS;
}

static void s_finish_provider_destroy(struct aws_s3express_credentials_provider *provider) {
    AWS_LOGF_TRACE(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): finishing destroying S3 Express credentials provider",
        (void *)provider);

    struct aws_s3express_credentials_provider_impl *impl = provider->impl;
    aws_hash_table_clean_up(&impl->synced_data.session_creator_table);
    aws_cache_destroy(impl->synced_data.cache);
    aws_credentials_release(impl->default_original_credentials);
    aws_credentials_provider_release(impl->default_original_credentials_provider);
    aws_mutex_clean_up(&impl->synced_data.lock);
    aws_mem_release(provider->allocator, impl->bg_refresh_task);
    /* Invoke provider shutdown callback */
    if (provider && provider->shutdown_complete_callback) {
        provider->shutdown_complete_callback(provider->shutdown_user_data);
    }
    aws_mem_release(provider->allocator, provider);
}

/* This is scheduled to run on the background task's event loop.  */
static void s_clean_up_background_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)status;
    struct aws_s3express_credentials_provider *provider = arg;
    struct aws_s3express_credentials_provider_impl *impl = provider->impl;

    /* Cancel the task will run the task synchronously */
    aws_event_loop_cancel_task(impl->bg_event_loop, impl->bg_refresh_task);
    aws_mem_release(provider->allocator, task);

    /* Safely remove the internal ref as the background task is killed. */
    aws_ref_count_release(&impl->internal_ref);
}

static void s_external_destroy(struct aws_s3express_credentials_provider *provider) {
    AWS_LOGF_TRACE(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): external refcount drops to zero, start destroying",
        (void *)provider);

    struct aws_s3express_credentials_provider_impl *impl = provider->impl;
    { /* BEGIN CRITICAL SECTION */
        s_credentials_provider_s3express_impl_lock_synced_data(impl);
        impl->synced_data.destroying = true;
        aws_cache_clear(impl->synced_data.cache);
        for (struct aws_hash_iter iter = aws_hash_iter_begin(&impl->synced_data.session_creator_table);
             !aws_hash_iter_done(&iter);
             aws_hash_iter_next(&iter)) {
            struct aws_s3express_session_creator *session_creator =
                (struct aws_s3express_session_creator *)iter.element.value;
            /* Cancel all meta requests */
            aws_s3_meta_request_cancel(session_creator->synced_data.meta_request);
        }
        s_credentials_provider_s3express_impl_unlock_synced_data(impl);
    } /* END CRITICAL SECTION */

    /* Clean up the background thread */
    struct aws_task *clean_up_background_task = aws_mem_calloc(provider->allocator, 1, sizeof(struct aws_task));
    aws_task_init(clean_up_background_task, s_clean_up_background_task, provider, "clean_up_s3express_background");
    aws_event_loop_schedule_task_now(impl->bg_event_loop, clean_up_background_task);
}

static struct aws_s3express_credentials_provider_vtable s_aws_s3express_credentials_provider_vtable = {
    .get_credentials = s_s3express_get_creds,
    .destroy = s_external_destroy,
};

static void s_schedule_bg_refresh(struct aws_s3express_credentials_provider *provider) {
    struct aws_s3express_credentials_provider_impl *impl = provider->impl;

    AWS_FATAL_ASSERT(impl->bg_event_loop != NULL);
    uint64_t current_stamp = UINT64_MAX;
    /* Use high res clock to schedule the task in the future. */
    aws_high_res_clock_get_ticks(&current_stamp);
    uint64_t interval_secs = impl->mock_test.bg_refresh_secs_override == 0 ? s_background_refresh_interval_secs
                                                                           : impl->mock_test.bg_refresh_secs_override;

    /* Schedule the refresh task to happen in the future. */
    aws_event_loop_schedule_task_future(
        impl->bg_event_loop,
        impl->bg_refresh_task,
        current_stamp + aws_timestamp_convert(interval_secs, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL));

    return;
}

static void s_refresh_session_list(
    struct aws_s3express_credentials_provider *provider,
    const struct aws_credentials *current_original_credentials) {

    struct aws_s3express_credentials_provider_impl *impl = provider->impl;
    uint64_t current_stamp = UINT64_MAX;
    aws_sys_clock_get_ticks(&current_stamp);
    uint64_t now_seconds = aws_timestamp_convert(current_stamp, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, NULL);
    AWS_LOGF_TRACE(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p): background refreshing task in process", (void *)provider);
    { /* BEGIN CRITICAL SECTION */
        s_credentials_provider_s3express_impl_lock_synced_data(impl);
        if (impl->synced_data.destroying) {
            /* Client is gone, stops doing anything */
            s_credentials_provider_s3express_impl_unlock_synced_data(impl);
            return;
        }
        const struct aws_linked_list *session_list =
            aws_linked_hash_table_get_iteration_list(&impl->synced_data.cache->table);
        /* Iterate through the cache without changing the priority */

        struct aws_linked_list_node *node = NULL;
        for (node = aws_linked_list_begin(session_list); node != aws_linked_list_end(session_list);) {
            /* Iterate through all nodes and clean the resource up */
            struct aws_linked_hash_table_node *table_node =
                AWS_CONTAINER_OF(node, struct aws_linked_hash_table_node, node);
            node = aws_linked_list_next(node);
            struct aws_s3express_session *session = table_node->value;
            if (s_s3express_session_about_to_expire(session, now_seconds)) {
                if (session->inactive) {
                    /* The session has been inactive since last refresh, remove it from the cache. */
                    aws_cache_remove(impl->synced_data.cache, session->hash_key);
                } else {
                    /* If we are about to expire, try to refresh the credentials */
                    /* Check the creator map */
                    struct aws_hash_element *session_creator_hash_element = NULL;
                    int was_created = 0;
                    struct aws_string *hash_key = aws_string_new_from_string(provider->allocator, session->hash_key);
                    int ret_code = aws_hash_table_create(
                        &impl->synced_data.session_creator_table,
                        hash_key,
                        &session_creator_hash_element,
                        &was_created);
                    AWS_ASSERT(ret_code == AWS_OP_SUCCESS);
                    (void)ret_code;
                    if (was_created) {
                        struct aws_string *current_creds_hash = aws_encode_s3express_hash_key_new(
                            provider->allocator,
                            current_original_credentials,
                            aws_byte_cursor_from_string(session->host));
                        bool creds_match = aws_string_eq(current_creds_hash, hash_key);
                        aws_string_destroy(current_creds_hash);
                        if (!creds_match) {
                            /* The session was created with a separate credentials, we skip refreshing it. */
                            if (!s_s3express_session_is_valid(session, now_seconds)) {
                                /* Purge the session when it is expired. */
                                aws_cache_remove(impl->synced_data.cache, session->hash_key);
                            }
                            /* Mark it as inactive, so that we can purge the session directly from next refresh */
                            session->inactive = true;
                            /* Remove the element we just created as we skip refrshing. */
                            aws_string_destroy(hash_key);
                            aws_hash_table_remove_element(
                                &impl->synced_data.session_creator_table, session_creator_hash_element);
                            goto unlock;
                        }

                        struct aws_credentials_properties_s3express s3express_properties = {
                            .host = aws_byte_cursor_from_string(session->host),
                        };
                        if (session->region) {
                            s3express_properties.region = aws_byte_cursor_from_string(session->region);
                        }
                        /* A new session creator needed to refresh the session */
                        struct aws_s3express_session_creator *new_session_creator =
                            s_session_creator_new(provider, current_original_credentials, &s3express_properties);
                        AWS_FATAL_ASSERT(new_session_creator);
                        new_session_creator->synced_data.session = session;
                        session->creator = new_session_creator;
                        new_session_creator->hash_key = hash_key;

                        session_creator_hash_element->value = new_session_creator;
                    } else {
                        /* The session is in process of refreshing. Only valid if the previous create session to
                         * refresh still not finished, otherwise, it's a bug */
                        aws_string_destroy(hash_key);
                        struct aws_s3express_session_creator *session_creator = session_creator_hash_element->value;
                        AWS_FATAL_ASSERT(session_creator->synced_data.session == session);
                    }
                    session->inactive = true;
                }
            }
        }
    unlock:
        s_credentials_provider_s3express_impl_unlock_synced_data(impl);
    } /* END CRITICAL SECTION */
    s_schedule_bg_refresh(provider);
}

static void s_get_original_credentials_callback(struct aws_credentials *credentials, int error_code, void *user_data) {
    struct aws_s3express_credentials_provider *provider = user_data;
    if (error_code) {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: S3 Express Provider back ground refresh failed: Failed to fetch original credentials with "
            "error %s. Skipping refresh.",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        /* Skip this refresh, but keep schedule the next one */
        s_schedule_bg_refresh(provider);
        return;
    }
    s_refresh_session_list(provider, credentials);
}

static void s_bg_refresh_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        return;
    }

    struct aws_s3express_credentials_provider *provider = arg;
    struct aws_s3express_credentials_provider_impl *impl = provider->impl;
    if (impl->default_original_credentials) {
        s_refresh_session_list(provider, impl->default_original_credentials);
    } else {
        /* Get the credentials from provider first. */
        if (aws_credentials_provider_get_credentials(
                impl->default_original_credentials_provider, s_get_original_credentials_callback, provider)) {
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "id=%p: S3 Express Provider back ground refresh failed: Failed to get original credentials from "
                "provider with error %s. Skipping refresh.",
                (void *)provider,
                aws_error_debug_str(aws_last_error()));
            /* Skip this refresh, but keep schedule the next one */
            s_schedule_bg_refresh(provider);
            return;
        }
    }
}

void aws_s3express_credentials_provider_init_base(
    struct aws_s3express_credentials_provider *provider,
    struct aws_allocator *allocator,
    struct aws_s3express_credentials_provider_vtable *vtable,
    void *impl) {

    AWS_PRECONDITION(provider);
    AWS_PRECONDITION(vtable);

    provider->allocator = allocator;
    provider->vtable = vtable;
    provider->impl = impl;
    aws_ref_count_init(&provider->ref_count, provider, (aws_simple_completion_callback *)provider->vtable->destroy);
}

struct aws_s3express_credentials_provider *aws_s3express_credentials_provider_new_default(
    struct aws_allocator *allocator,
    const struct aws_s3express_credentials_provider_default_options *options) {

    if (!options->client) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "a S3 client is necessary for querying S3 Express");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_s3express_credentials_provider *provider = NULL;
    struct aws_s3express_credentials_provider_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_s3express_credentials_provider),
        &impl,
        sizeof(struct aws_s3express_credentials_provider_impl));

    AWS_LOGF_DEBUG(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: creating S3 Express credentials provider");
    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_s3express_credentials_provider_init_base(
        provider, allocator, &s_aws_s3express_credentials_provider_vtable, impl);

    aws_hash_table_init(
        &impl->synced_data.session_creator_table,
        allocator,
        10,
        aws_hash_string,
        aws_hash_callback_string_eq,
        NULL,
        NULL);

    impl->synced_data.cache = aws_cache_new_lru(
        allocator,
        aws_hash_string,
        (aws_hash_callback_eq_fn *)aws_string_eq,
        NULL,
        (aws_hash_callback_destroy_fn *)s_aws_s3express_session_destroy,
        s_default_cache_capacity);
    AWS_ASSERT(impl->synced_data.cache);

    /* Not keep the s3 client alive to avoid recursive reference */
    impl->client = options->client;
    struct aws_signing_config_aws client_cached_config = impl->client->cached_signing_config->config;
    if (client_cached_config.credentials) {
        impl->default_original_credentials = client_cached_config.credentials;
        aws_credentials_acquire(impl->default_original_credentials);
    } else {
        impl->default_original_credentials_provider =
            aws_credentials_provider_acquire(client_cached_config.credentials_provider);
    }

    provider->shutdown_complete_callback = options->shutdown_complete_callback;
    provider->shutdown_user_data = options->shutdown_user_data;
    aws_mutex_init(&impl->synced_data.lock);
    aws_ref_count_init(&impl->internal_ref, provider, (aws_simple_completion_callback *)s_finish_provider_destroy);

    /* Init the background refresh task */
    impl->bg_refresh_task = aws_mem_calloc(provider->allocator, 1, sizeof(struct aws_task));
    aws_task_init(impl->bg_refresh_task, s_bg_refresh_task, provider, "s3express_background_refresh");
    /* Get an event loop from the client */
    impl->bg_event_loop = aws_event_loop_group_get_next_loop(impl->client->client_bootstrap->event_loop_group);
    impl->mock_test.bg_refresh_secs_override = options->mock_test.bg_refresh_secs_override;
    s_schedule_bg_refresh(provider);

    return provider;
}

struct aws_s3express_credentials_provider *aws_s3express_credentials_provider_release(
    struct aws_s3express_credentials_provider *provider) {
    if (provider) {
        aws_ref_count_release(&provider->ref_count);
    }
    return NULL;
}

int aws_s3express_credentials_provider_get_credentials(
    struct aws_s3express_credentials_provider *provider,
    const struct aws_credentials *original_credentials,
    const struct aws_credentials_properties_s3express *property,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    AWS_PRECONDITION(property);
    AWS_PRECONDITION(provider);
    AWS_ASSERT(provider->vtable->get_credentials);

    return provider->vtable->get_credentials(provider, original_credentials, property, callback, user_data);
}
