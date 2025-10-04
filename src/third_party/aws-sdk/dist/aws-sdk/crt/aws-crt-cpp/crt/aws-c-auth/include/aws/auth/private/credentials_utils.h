#ifndef AWS_AUTH_CREDENTIALS_PRIVATE_H
#define AWS_AUTH_CREDENTIALS_PRIVATE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/auth.h>
#include <aws/auth/credentials.h>
#include <aws/http/connection_manager.h>
#include <aws/io/retry_strategy.h>

struct aws_http_connection;
struct aws_http_connection_manager;
struct aws_http_make_request_options;
struct aws_http_stream;
struct aws_json_value;
struct aws_profile;

/*
 * Internal struct tracking an asynchronous credentials query.
 * Used by both the cached provider and the test mocks.
 *
 */
struct aws_credentials_query {
    struct aws_linked_list_node node;
    struct aws_credentials_provider *provider;
    aws_on_get_credentials_callback_fn *callback;
    void *user_data;
};

typedef struct aws_http_connection_manager *(
    aws_http_connection_manager_new_fn)(struct aws_allocator *allocator,
                                        const struct aws_http_connection_manager_options *options);
typedef void(aws_http_connection_manager_release_fn)(struct aws_http_connection_manager *manager);
typedef void(aws_http_connection_manager_acquire_connection_fn)(
    struct aws_http_connection_manager *manager,
    aws_http_connection_manager_on_connection_setup_fn *callback,
    void *user_data);
typedef int(aws_http_connection_manager_release_connection_fn)(
    struct aws_http_connection_manager *manager,
    struct aws_http_connection *connection);
typedef struct aws_http_stream *(
    aws_http_connection_make_request_fn)(struct aws_http_connection *client_connection,
                                         const struct aws_http_make_request_options *options);
typedef int(aws_http_stream_activate_fn)(struct aws_http_stream *stream);
typedef struct aws_http_connection *(aws_http_stream_get_connection_fn)(const struct aws_http_stream *stream);

typedef int(aws_http_stream_get_incoming_response_status_fn)(const struct aws_http_stream *stream, int *out_status);
typedef void(aws_http_stream_release_fn)(struct aws_http_stream *stream);
typedef void(aws_http_connection_close_fn)(struct aws_http_connection *connection);

/*
 * Table of all downstream http functions used by the credentials providers that make http calls. Allows for simple
 * mocking.
 */
struct aws_auth_http_system_vtable {
    aws_http_connection_manager_new_fn *aws_http_connection_manager_new;
    aws_http_connection_manager_release_fn *aws_http_connection_manager_release;

    aws_http_connection_manager_acquire_connection_fn *aws_http_connection_manager_acquire_connection;
    aws_http_connection_manager_release_connection_fn *aws_http_connection_manager_release_connection;

    aws_http_connection_make_request_fn *aws_http_connection_make_request;
    aws_http_stream_activate_fn *aws_http_stream_activate;
    aws_http_stream_get_connection_fn *aws_http_stream_get_connection;
    aws_http_stream_get_incoming_response_status_fn *aws_http_stream_get_incoming_response_status;
    aws_http_stream_release_fn *aws_http_stream_release;

    aws_http_connection_close_fn *aws_http_connection_close;

    int (*aws_high_res_clock_get_ticks)(uint64_t *timestamp);
};

enum aws_parse_credentials_expiration_format {
    AWS_PCEF_STRING_ISO_8601_DATE,
    AWS_PCEF_NUMBER_UNIX_EPOCH,
    AWS_PCEF_NUMBER_UNIX_EPOCH_MS,
};

struct aws_parse_credentials_from_json_doc_options {
    const char *access_key_id_name;
    const char *secret_access_key_name;
    const char *token_name;
    const char *expiration_name;
    const char *top_level_object_name;
    enum aws_parse_credentials_expiration_format expiration_format;
    bool token_required;
    bool expiration_required;
};

AWS_EXTERN_C_BEGIN

/*
 * Misc. credentials-related APIs
 */

AWS_AUTH_API
void aws_credentials_query_init(
    struct aws_credentials_query *query,
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn *callback,
    void *user_data);

AWS_AUTH_API
void aws_credentials_query_clean_up(struct aws_credentials_query *query);

AWS_AUTH_API
void aws_credentials_provider_init_base(
    struct aws_credentials_provider *provider,
    struct aws_allocator *allocator,
    struct aws_credentials_provider_vtable *vtable,
    void *impl);

AWS_AUTH_API
void aws_credentials_provider_destroy(struct aws_credentials_provider *provider);

AWS_AUTH_API
void aws_credentials_provider_invoke_shutdown_callback(struct aws_credentials_provider *provider);

/**
 * This API is used internally to parse credentials from json document.
 * It _ONLY_ parses the first level of json structure. json document like
 * this will produce a valid credentials:
 {
    "accessKeyId" : "...",
    "secretAccessKey" : "...",
    "Token" : "...",
    "expiration" : "2019-05-29T00:21:43Z"
 }
 * but json document like this won't:
 {
    "credentials": {
        "accessKeyId" : "...",
        "secretAccessKey" : "...",
        "sessionToken" : "...",
        "expiration" : "2019-05-29T00:21:43Z"
    }
 }
 * In general, the keys' names of credentials in json document are:
 * "AccessKeyId", "SecretAccessKey", "Token" and "Expiration",
 * but there are cases services use different keys like "sessionToken".
 * A valid credentials must have "access key" and "secrete access key".
 * For some services, token and expiration are not required.
 * So in this API, the keys are provided by callers and this API will
 * perform a case insensitive search.
 */
AWS_AUTH_API
struct aws_credentials *aws_parse_credentials_from_aws_json_object(
    struct aws_allocator *allocator,
    struct aws_json_value *document_root,
    const struct aws_parse_credentials_from_json_doc_options *options);

/**
 * This API is similar to aws_parse_credentials_from_aws_json_object,
 * except it accepts a char buffer json document as it's input.
 */
AWS_AUTH_API
struct aws_credentials *aws_parse_credentials_from_json_document(
    struct aws_allocator *allocator,
    struct aws_byte_cursor json_document,
    const struct aws_parse_credentials_from_json_doc_options *options);

AWS_AUTH_API
enum aws_retry_error_type aws_credentials_provider_compute_retry_error_type(int response_code, int error_code);

/*
 * Constructs an endpoint in the format of service_name.region.amazonaws.com
 * If the region is cn-north-1 or cn-northwest-1, .cn is appended to support China-specific regional endpoints.
 */
AWS_AUTH_API
int aws_credentials_provider_construct_regional_endpoint(
    struct aws_allocator *allocator,
    struct aws_string **out_endpoint,
    const struct aws_string *region,
    const struct aws_string *service_name);

/*
 * Loads an aws config profile collection
 */
AWS_AUTH_API
struct aws_profile_collection *aws_load_profile_collection_from_config_file(
    struct aws_allocator *allocator,
    struct aws_byte_cursor config_file_name_override);

/*
 * Resolve region from environment in the following order:
 * 1. AWS_REGION
 * 2. AWS_DEFAULT_REGION
 */
AWS_AUTH_API
struct aws_string *aws_credentials_provider_resolve_region_from_env(struct aws_allocator *allocator);

AWS_EXTERN_C_END

#endif /* AWS_AUTH_CREDENTIALS_PRIVATE_H */
