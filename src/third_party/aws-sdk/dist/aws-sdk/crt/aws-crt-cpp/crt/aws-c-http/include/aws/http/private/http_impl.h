#ifndef AWS_HTTP_IMPL_H
#define AWS_HTTP_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

/**
 * Methods that affect internal processing.
 * This is NOT a definitive list of methods.
 */
enum aws_http_method {
    AWS_HTTP_METHOD_UNKNOWN, /* Unrecognized value. */
    AWS_HTTP_METHOD_GET,
    AWS_HTTP_METHOD_HEAD,
    AWS_HTTP_METHOD_CONNECT,
    AWS_HTTP_METHOD_COUNT, /* Number of enums */
};

/**
 * Headers that affect internal processing.
 * This is NOT a definitive list of headers.
 */
enum aws_http_header_name {
    AWS_HTTP_HEADER_UNKNOWN, /* Unrecognized value */

    /* Request pseudo-headers */
    AWS_HTTP_HEADER_METHOD,
    AWS_HTTP_HEADER_SCHEME,
    AWS_HTTP_HEADER_AUTHORITY,
    AWS_HTTP_HEADER_PATH,

    /* Response pseudo-headers */
    AWS_HTTP_HEADER_STATUS,

    /* Regular headers */
    AWS_HTTP_HEADER_CONNECTION,
    AWS_HTTP_HEADER_CONTENT_LENGTH,
    AWS_HTTP_HEADER_EXPECT,
    AWS_HTTP_HEADER_TRANSFER_ENCODING,
    AWS_HTTP_HEADER_COOKIE,
    AWS_HTTP_HEADER_SET_COOKIE,
    AWS_HTTP_HEADER_HOST,
    AWS_HTTP_HEADER_CACHE_CONTROL,
    AWS_HTTP_HEADER_MAX_FORWARDS,
    AWS_HTTP_HEADER_PRAGMA,
    AWS_HTTP_HEADER_RANGE,
    AWS_HTTP_HEADER_TE,
    AWS_HTTP_HEADER_CONTENT_ENCODING,
    AWS_HTTP_HEADER_CONTENT_TYPE,
    AWS_HTTP_HEADER_CONTENT_RANGE,
    AWS_HTTP_HEADER_TRAILER,
    AWS_HTTP_HEADER_WWW_AUTHENTICATE,
    AWS_HTTP_HEADER_AUTHORIZATION,
    AWS_HTTP_HEADER_PROXY_AUTHENTICATE,
    AWS_HTTP_HEADER_PROXY_AUTHORIZATION,
    AWS_HTTP_HEADER_AGE,
    AWS_HTTP_HEADER_EXPIRES,
    AWS_HTTP_HEADER_DATE,
    AWS_HTTP_HEADER_LOCATION,
    AWS_HTTP_HEADER_RETRY_AFTER,
    AWS_HTTP_HEADER_VARY,
    AWS_HTTP_HEADER_WARNING,
    AWS_HTTP_HEADER_UPGRADE,
    AWS_HTTP_HEADER_KEEP_ALIVE,
    AWS_HTTP_HEADER_PROXY_CONNECTION,

    AWS_HTTP_HEADER_COUNT, /* Number of enums */
};

AWS_EXTERN_C_BEGIN

AWS_HTTP_API void aws_http_fatal_assert_library_initialized(void);

AWS_HTTP_API struct aws_byte_cursor aws_http_version_to_str(enum aws_http_version version);

/**
 * Returns appropriate enum, or AWS_HTTP_METHOD_UNKNOWN if no match found.
 * Case-sensitive
 */
AWS_HTTP_API enum aws_http_method aws_http_str_to_method(struct aws_byte_cursor cursor);

/**
 * Returns appropriate enum, or AWS_HTTP_HEADER_UNKNOWN if no match found.
 * Not case-sensitive
 */
AWS_HTTP_API enum aws_http_header_name aws_http_str_to_header_name(struct aws_byte_cursor cursor);

/**
 * Returns appropriate enum, or AWS_HTTP_HEADER_UNKNOWN if no match found.
 * Case-sensitive (ex: "Connection" -> AWS_HTTP_HEADER_UNKNOWN because we looked for "connection").
 */
AWS_HTTP_API enum aws_http_header_name aws_http_lowercase_str_to_header_name(struct aws_byte_cursor cursor);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_IMPL_H */
