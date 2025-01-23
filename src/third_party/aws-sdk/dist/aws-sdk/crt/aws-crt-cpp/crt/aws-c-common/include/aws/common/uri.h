#ifndef AWS_COMMON_URI_H
#define AWS_COMMON_URI_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>

AWS_PUSH_SANE_WARNING_LEVEL

/**
 * Data representing a URI. uri_str is always allocated and filled in.
 * The other portions are merely storing offsets into uri_str.
 */
struct aws_uri {
    size_t self_size;
    struct aws_allocator *allocator;
    struct aws_byte_buf uri_str;
    struct aws_byte_cursor scheme;
    struct aws_byte_cursor authority;
    struct aws_byte_cursor userinfo;
    struct aws_byte_cursor user;
    struct aws_byte_cursor password;
    struct aws_byte_cursor host_name;
    uint32_t port;
    struct aws_byte_cursor path;
    struct aws_byte_cursor query_string;
    struct aws_byte_cursor path_and_query;
};

/**
 * key/value pairs for a query string. If the query fragment was not in format key=value, the fragment value
 * will be stored in key
 */
struct aws_uri_param {
    struct aws_byte_cursor key;
    struct aws_byte_cursor value;
};

/**
 * Arguments for building a URI instance. All members must
 * be initialized before passing them to aws_uri_init().
 *
 * query_string and query_params are exclusive to each other. If you set
 * query_string, do not prepend it with '?'
 */
struct aws_uri_builder_options {
    struct aws_byte_cursor scheme;
    struct aws_byte_cursor path;
    struct aws_byte_cursor host_name;
    uint32_t port;
    struct aws_array_list *query_params;
    struct aws_byte_cursor query_string;
};

AWS_EXTERN_C_BEGIN

/**
 * Parses 'uri_str' and initializes uri. Returns AWS_OP_SUCCESS, on success, AWS_OP_ERR on failure.
 * After calling this function, the parts can be accessed.
 */
AWS_COMMON_API int aws_uri_init_parse(
    struct aws_uri *uri,
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *uri_str);

/**
 * Initializes uri to values specified in options. Returns AWS_OP_SUCCESS, on success, AWS_OP_ERR on failure.
 * After calling this function, the parts can be accessed.
 */
AWS_COMMON_API int aws_uri_init_from_builder_options(
    struct aws_uri *uri,
    struct aws_allocator *allocator,
    struct aws_uri_builder_options *options);
AWS_COMMON_API void aws_uri_clean_up(struct aws_uri *uri);

/**
 * Returns the scheme portion of the uri (e.g. http, https, ftp, ftps, etc...). If the scheme was not present
 * in the uri, the returned value will be empty. It is the users job to determine the appropriate defaults
 * if this field is empty, based on protocol, port, etc...
 */
AWS_COMMON_API const struct aws_byte_cursor *aws_uri_scheme(const struct aws_uri *uri);

/**
 * Returns the authority portion of the uri (host[:port]). If it was not present, this was a request uri. In that
 * case, the value will be empty.
 */
AWS_COMMON_API const struct aws_byte_cursor *aws_uri_authority(const struct aws_uri *uri);

/**
 * Returns the path portion of the uri, including any leading '/'. If not present, this value will be empty.
 */
AWS_COMMON_API const struct aws_byte_cursor *aws_uri_path(const struct aws_uri *uri);

/**
 * Returns the query string portion of the uri, minus the '?'. If not present, this value will be empty.
 */
AWS_COMMON_API const struct aws_byte_cursor *aws_uri_query_string(const struct aws_uri *uri);

/**
 * Returns the 'host_name' portion of the authority. If no authority was present, this value will be empty.
 */
AWS_COMMON_API const struct aws_byte_cursor *aws_uri_host_name(const struct aws_uri *uri);

/**
 * Returns the port portion of the authority if it was present, otherwise, returns 0.
 * If this is 0, it is the users job to determine the correct port based on scheme and protocol.
 */
AWS_COMMON_API uint32_t aws_uri_port(const struct aws_uri *uri);

/**
 * Returns the path and query portion of the uri (i.e., the thing you send across the wire).
 */
AWS_COMMON_API const struct aws_byte_cursor *aws_uri_path_and_query(const struct aws_uri *uri);

/**
 * For iterating over the params in the query string.
 * `param` is an in/out argument used to track progress, it MUST be zeroed out to start.
 * If true is returned, `param` contains the value of the next param.
 * If false is returned, there are no further params.
 *
 * Edge cases:
 * 1) Entries without '=' sign are treated as having a key and no value.
 *    Example: First param in query string "a&b=c" has key="a" value=""
 *
 * 2) Blank entries are skipped.
 *    Example: The only param in query string "&&a=b" is key="a" value="b"
 */
AWS_COMMON_API bool aws_query_string_next_param(struct aws_byte_cursor query_string, struct aws_uri_param *param);

/**
 * Parses query string and stores the parameters in 'out_params'. Returns AWS_OP_SUCCESS on success and
 * AWS_OP_ERR on failure. The user is responsible for initializing out_params with item size of struct aws_query_param.
 * The user is also responsible for cleaning up out_params when finished.
 */
AWS_COMMON_API int aws_query_string_params(struct aws_byte_cursor query_string, struct aws_array_list *out_params);

/**
 * For iterating over the params in the uri query string.
 * `param` is an in/out argument used to track progress, it MUST be zeroed out to start.
 * If true is returned, `param` contains the value of the next param.
 * If false is returned, there are no further params.
 *
 * Edge cases:
 * 1) Entries without '=' sign are treated as having a key and no value.
 *    Example: First param in query string "a&b=c" has key="a" value=""
 *
 * 2) Blank entries are skipped.
 *    Example: The only param in query string "&&a=b" is key="a" value="b"
 */
AWS_COMMON_API bool aws_uri_query_string_next_param(const struct aws_uri *uri, struct aws_uri_param *param);

/**
 * Parses query string and stores the parameters in 'out_params'. Returns AWS_OP_SUCCESS on success and
 * AWS_OP_ERR on failure. The user is responsible for initializing out_params with item size of struct aws_query_param.
 * The user is also responsible for cleaning up out_params when finished.
 */
AWS_COMMON_API int aws_uri_query_string_params(const struct aws_uri *uri, struct aws_array_list *out_params);

/**
 * Writes the uri path encoding of a cursor to a buffer.  This is the modified version of rfc3986 used by
 * sigv4 signing.
 */
AWS_COMMON_API int aws_byte_buf_append_encoding_uri_path(
    struct aws_byte_buf *buffer,
    const struct aws_byte_cursor *cursor);

/**
 * Writes the uri query param encoding (passthrough alnum + '-' '_' '~' '.') of a UTF-8 cursor to a buffer
 * For example, reading "a b_c" would write "a%20b_c".
 */
AWS_COMMON_API int aws_byte_buf_append_encoding_uri_param(
    struct aws_byte_buf *buffer,
    const struct aws_byte_cursor *cursor);

/**
 * Writes the uri decoding of a UTF-8 cursor to a buffer,
 * replacing %xx escapes by their single byte equivalent.
 * For example, reading "a%20b_c" would write "a b_c".
 */
AWS_COMMON_API int aws_byte_buf_append_decoding_uri(struct aws_byte_buf *buffer, const struct aws_byte_cursor *cursor);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_URI_H */
