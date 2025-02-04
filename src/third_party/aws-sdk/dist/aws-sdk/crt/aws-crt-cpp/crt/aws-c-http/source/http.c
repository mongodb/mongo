/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/hash_table.h>
#include <aws/compression/compression.h>
#include <aws/http/private/hpack.h>
#include <aws/http/private/http_impl.h>
#include <aws/http/status_code.h>
#include <aws/io/logging.h>

#include <ctype.h>

#define AWS_DEFINE_ERROR_INFO_HTTP(CODE, STR) [(CODE) - 0x0800] = AWS_DEFINE_ERROR_INFO(CODE, STR, "aws-c-http")

/* clang-format off */
static struct aws_error_info s_errors[] = {
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_UNKNOWN,
        "Encountered an unknown error."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_HEADER_NOT_FOUND,
        "The specified header was not found"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_INVALID_HEADER_FIELD,
        "Invalid header field, including a forbidden header field."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_INVALID_HEADER_NAME,
        "Invalid header name."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_INVALID_HEADER_VALUE,
        "Invalid header value."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_INVALID_METHOD,
        "Method is invalid."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_INVALID_PATH,
        "Path is invalid."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_INVALID_STATUS_CODE,
        "Status code is invalid."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_MISSING_BODY_STREAM,
        "Given the provided headers (ex: Content-Length), a body is expected."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_INVALID_BODY_STREAM,
        "A body stream provided, but the message does not allow body (ex: response for HEAD Request and 304 response)"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_CONNECTION_CLOSED,
        "The connection has closed or is closing."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_SWITCHED_PROTOCOLS,
        "The connection has switched protocols."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_UNSUPPORTED_PROTOCOL,
        "An unsupported protocol was encountered."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_REACTION_REQUIRED,
        "A necessary function was not invoked from a user callback."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_DATA_NOT_AVAILABLE,
        "This data is not yet available."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_OUTGOING_STREAM_LENGTH_INCORRECT,
        "Amount of data streamed out does not match the previously declared length."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_CALLBACK_FAILURE,
        "A callback has reported failure."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE,
        "Failed to upgrade HTTP connection to Websocket."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_WEBSOCKET_CLOSE_FRAME_SENT,
        "Websocket has sent CLOSE frame, no more data will be sent."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_WEBSOCKET_IS_MIDCHANNEL_HANDLER,
        "Operation cannot be performed because websocket has been converted to a midchannel handler."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_CONNECTION_MANAGER_INVALID_STATE_FOR_ACQUIRE,
        "Acquire called after the connection manager's ref count has reached zero"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_CONNECTION_MANAGER_VENDED_CONNECTION_UNDERFLOW,
        "Release called when the connection manager's vended connection count was zero"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_SERVER_CLOSED,
        "The http server is closed, no more connections will be accepted"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_PROXY_CONNECT_FAILED,
        "Proxy-based connection establishment failed because the CONNECT call failed"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_CONNECTION_MANAGER_SHUTTING_DOWN,
        "Connection acquisition failed because connection manager is shutting down"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_CHANNEL_THROUGHPUT_FAILURE,
        "Http connection channel shut down due to failure to meet throughput minimum"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_PROTOCOL_ERROR,
        "Protocol rules violated by peer"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_STREAM_IDS_EXHAUSTED,
        "Connection exhausted all possible HTTP-stream IDs. Establish a new connection for new streams."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_GOAWAY_RECEIVED,
        "Peer sent GOAWAY to initiate connection shutdown. Establish a new connection to retry the HTTP-streams."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_RST_STREAM_RECEIVED,
        "Peer sent RST_STREAM to terminate HTTP-stream."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_RST_STREAM_SENT,
        "RST_STREAM has sent from local implementation and HTTP-stream has been terminated."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_STREAM_NOT_ACTIVATED,
        "HTTP-stream must be activated before use."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_STREAM_HAS_COMPLETED,
        "HTTP-stream has completed, action cannot be performed."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_PROXY_STRATEGY_NTLM_CHALLENGE_TOKEN_MISSING,
        "NTLM Proxy strategy was initiated without a challenge token"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_PROXY_STRATEGY_TOKEN_RETRIEVAL_FAILURE,
        "Failure in user code while retrieving proxy auth token"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_PROXY_CONNECT_FAILED_RETRYABLE,
        "Proxy connection attempt failed but the negotiation could be continued on a new connection"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_PROTOCOL_SWITCH_FAILURE,
        "Internal state failure prevent connection from switching protocols"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_MAX_CONCURRENT_STREAMS_EXCEEDED,
        "Max concurrent stream reached"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_STREAM_MANAGER_SHUTTING_DOWN,
        "Stream acquisition failed because stream manager is shutting down"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_STREAM_MANAGER_CONNECTION_ACQUIRE_FAILURE,
        "Stream acquisition failed because stream manager failed to acquire a connection"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_STREAM_MANAGER_UNEXPECTED_HTTP_VERSION,
        "Stream acquisition failed because stream manager got an unexpected version of HTTP connection"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_WEBSOCKET_PROTOCOL_ERROR,
        "Websocket protocol rules violated by peer"),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_MANUAL_WRITE_NOT_ENABLED,
        "Manual write failed because manual writes are not enabled."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_MANUAL_WRITE_HAS_COMPLETED,
        "Manual write failed because manual writes are already completed."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_RESPONSE_FIRST_BYTE_TIMEOUT,
        "Timed out waiting for first byte of HTTP response, after sending the full request."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_CONNECTION_MANAGER_ACQUISITION_TIMEOUT,
        "Connection Manager failed to acquire a connection within the defined timeout."),
    AWS_DEFINE_ERROR_INFO_HTTP(
        AWS_ERROR_HTTP_CONNECTION_MANAGER_MAX_PENDING_ACQUISITIONS_EXCEEDED,
        "Max pending acquisitions reached"),
};
/* clang-format on */

static struct aws_error_info_list s_error_list = {
    .error_list = s_errors,
    .count = AWS_ARRAY_SIZE(s_errors),
};

static struct aws_log_subject_info s_log_subject_infos[] = {
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_GENERAL, "http", "Misc HTTP logging"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_CONNECTION, "http-connection", "HTTP client or server connection"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_ENCODER, "http-encoder", "HTTP data encoder"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_DECODER, "http-decoder", "HTTP data decoder"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_SERVER, "http-server", "HTTP server socket listening for incoming connections"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_STREAM, "http-stream", "HTTP request-response exchange"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_CONNECTION_MANAGER, "connection-manager", "HTTP connection manager"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_STREAM_MANAGER, "http2-stream-manager", "HTTP/2 stream manager"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_WEBSOCKET, "websocket", "Websocket"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_HTTP_WEBSOCKET_SETUP, "websocket-setup", "Websocket setup"),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_HTTP_PROXY_NEGOTIATION,
        "proxy-negotiation",
        "Negotiating an http connection with a proxy server"),
};

static struct aws_log_subject_info_list s_log_subject_list = {
    .subject_list = s_log_subject_infos,
    .count = AWS_ARRAY_SIZE(s_log_subject_infos),
};

struct aws_enum_value {
    struct aws_allocator *allocator;
    int value;
};

static void s_destroy_enum_value(void *value) {
    struct aws_enum_value *enum_value = value;
    aws_mem_release(enum_value->allocator, enum_value);
}

/**
 * Given array of aws_byte_cursors, init hashtable where...
 * Key is aws_byte_cursor* (pointing into cursor from array) and comparisons are case-insensitive.
 * Value is the array index cast to a void*.
 */
static void s_init_str_to_enum_hash_table(
    struct aws_hash_table *table,
    struct aws_allocator *alloc,
    struct aws_byte_cursor *str_array,
    int start_index,
    int end_index,
    bool ignore_case) {

    int err = aws_hash_table_init(
        table,
        alloc,
        end_index - start_index,
        ignore_case ? aws_hash_byte_cursor_ptr_ignore_case : aws_hash_byte_cursor_ptr,
        (aws_hash_callback_eq_fn *)(ignore_case ? aws_byte_cursor_eq_ignore_case : aws_byte_cursor_eq),
        NULL,
        s_destroy_enum_value);
    AWS_FATAL_ASSERT(!err);

    for (int i = start_index; i < end_index; ++i) {
        int was_created = 0;
        struct aws_enum_value *enum_value = aws_mem_calloc(alloc, 1, sizeof(struct aws_enum_value));
        AWS_FATAL_ASSERT(enum_value);
        enum_value->allocator = alloc;
        enum_value->value = i;

        AWS_FATAL_ASSERT(str_array[i].ptr && "Missing enum string");
        err = aws_hash_table_put(table, &str_array[i], (void *)enum_value, &was_created);
        AWS_FATAL_ASSERT(!err && was_created);
    }
}

/**
 * Given key, get value from table initialized by s_init_str_to_enum_hash_table().
 * Returns -1 if key not found.
 */
static int s_find_in_str_to_enum_hash_table(const struct aws_hash_table *table, struct aws_byte_cursor *key) {
    struct aws_hash_element *elem;
    aws_hash_table_find(table, key, &elem);
    if (elem) {
        struct aws_enum_value *enum_value = elem->value;
        return enum_value->value;
    }
    return -1;
}

/* METHODS */
static struct aws_hash_table s_method_str_to_enum;                         /* for string -> enum lookup */
static struct aws_byte_cursor s_method_enum_to_str[AWS_HTTP_METHOD_COUNT]; /* for enum -> string lookup */

static void s_methods_init(struct aws_allocator *alloc) {
    s_method_enum_to_str[AWS_HTTP_METHOD_GET] = aws_http_method_get;
    s_method_enum_to_str[AWS_HTTP_METHOD_HEAD] = aws_http_method_head;
    s_method_enum_to_str[AWS_HTTP_METHOD_CONNECT] = aws_http_method_connect;

    s_init_str_to_enum_hash_table(
        &s_method_str_to_enum,
        alloc,
        s_method_enum_to_str,
        AWS_HTTP_METHOD_UNKNOWN + 1,
        AWS_HTTP_METHOD_COUNT,
        false /* DO NOT ignore case of method */);
}

static void s_methods_clean_up(void) {
    aws_hash_table_clean_up(&s_method_str_to_enum);
}

enum aws_http_method aws_http_str_to_method(struct aws_byte_cursor cursor) {
    int method = s_find_in_str_to_enum_hash_table(&s_method_str_to_enum, &cursor);
    if (method >= 0) {
        return (enum aws_http_method)method;
    }
    return AWS_HTTP_METHOD_UNKNOWN;
}

/* VERSIONS */
static struct aws_byte_cursor s_version_enum_to_str[AWS_HTTP_HEADER_COUNT]; /* for enum -> string lookup */

static void s_versions_init(struct aws_allocator *alloc) {
    (void)alloc;
    s_version_enum_to_str[AWS_HTTP_VERSION_UNKNOWN] = aws_byte_cursor_from_c_str("Unknown");
    s_version_enum_to_str[AWS_HTTP_VERSION_1_0] = aws_byte_cursor_from_c_str("HTTP/1.0");
    s_version_enum_to_str[AWS_HTTP_VERSION_1_1] = aws_byte_cursor_from_c_str("HTTP/1.1");
    s_version_enum_to_str[AWS_HTTP_VERSION_2] = aws_byte_cursor_from_c_str("HTTP/2");
}

static void s_versions_clean_up(void) {}

struct aws_byte_cursor aws_http_version_to_str(enum aws_http_version version) {
    if ((int)version < AWS_HTTP_VERSION_UNKNOWN || (int)version >= AWS_HTTP_VERSION_COUNT) {
        version = AWS_HTTP_VERSION_UNKNOWN;
    }

    return s_version_enum_to_str[version];
}

/* HEADERS */
static struct aws_hash_table s_header_str_to_enum;           /* for case-insensitive string -> enum lookup */
static struct aws_hash_table s_lowercase_header_str_to_enum; /* for case-sensitive string -> enum lookup */
static struct aws_byte_cursor s_header_enum_to_str[AWS_HTTP_HEADER_COUNT]; /* for enum -> string lookup */

static void s_headers_init(struct aws_allocator *alloc) {
    s_header_enum_to_str[AWS_HTTP_HEADER_METHOD] = aws_byte_cursor_from_c_str(":method");
    s_header_enum_to_str[AWS_HTTP_HEADER_SCHEME] = aws_byte_cursor_from_c_str(":scheme");
    s_header_enum_to_str[AWS_HTTP_HEADER_AUTHORITY] = aws_byte_cursor_from_c_str(":authority");
    s_header_enum_to_str[AWS_HTTP_HEADER_PATH] = aws_byte_cursor_from_c_str(":path");
    s_header_enum_to_str[AWS_HTTP_HEADER_STATUS] = aws_byte_cursor_from_c_str(":status");
    s_header_enum_to_str[AWS_HTTP_HEADER_COOKIE] = aws_byte_cursor_from_c_str("cookie");
    s_header_enum_to_str[AWS_HTTP_HEADER_SET_COOKIE] = aws_byte_cursor_from_c_str("set-cookie");
    s_header_enum_to_str[AWS_HTTP_HEADER_HOST] = aws_byte_cursor_from_c_str("host");
    s_header_enum_to_str[AWS_HTTP_HEADER_CONNECTION] = aws_byte_cursor_from_c_str("connection");
    s_header_enum_to_str[AWS_HTTP_HEADER_CONTENT_LENGTH] = aws_byte_cursor_from_c_str("content-length");
    s_header_enum_to_str[AWS_HTTP_HEADER_EXPECT] = aws_byte_cursor_from_c_str("expect");
    s_header_enum_to_str[AWS_HTTP_HEADER_TRANSFER_ENCODING] = aws_byte_cursor_from_c_str("transfer-encoding");
    s_header_enum_to_str[AWS_HTTP_HEADER_CACHE_CONTROL] = aws_byte_cursor_from_c_str("cache-control");
    s_header_enum_to_str[AWS_HTTP_HEADER_MAX_FORWARDS] = aws_byte_cursor_from_c_str("max-forwards");
    s_header_enum_to_str[AWS_HTTP_HEADER_PRAGMA] = aws_byte_cursor_from_c_str("pragma");
    s_header_enum_to_str[AWS_HTTP_HEADER_RANGE] = aws_byte_cursor_from_c_str("range");
    s_header_enum_to_str[AWS_HTTP_HEADER_TE] = aws_byte_cursor_from_c_str("te");
    s_header_enum_to_str[AWS_HTTP_HEADER_CONTENT_ENCODING] = aws_byte_cursor_from_c_str("content-encoding");
    s_header_enum_to_str[AWS_HTTP_HEADER_CONTENT_TYPE] = aws_byte_cursor_from_c_str("content-type");
    s_header_enum_to_str[AWS_HTTP_HEADER_CONTENT_RANGE] = aws_byte_cursor_from_c_str("content-range");
    s_header_enum_to_str[AWS_HTTP_HEADER_TRAILER] = aws_byte_cursor_from_c_str("trailer");
    s_header_enum_to_str[AWS_HTTP_HEADER_WWW_AUTHENTICATE] = aws_byte_cursor_from_c_str("www-authenticate");
    s_header_enum_to_str[AWS_HTTP_HEADER_AUTHORIZATION] = aws_byte_cursor_from_c_str("authorization");
    s_header_enum_to_str[AWS_HTTP_HEADER_PROXY_AUTHENTICATE] = aws_byte_cursor_from_c_str("proxy-authenticate");
    s_header_enum_to_str[AWS_HTTP_HEADER_PROXY_AUTHORIZATION] = aws_byte_cursor_from_c_str("proxy-authorization");
    s_header_enum_to_str[AWS_HTTP_HEADER_AGE] = aws_byte_cursor_from_c_str("age");
    s_header_enum_to_str[AWS_HTTP_HEADER_EXPIRES] = aws_byte_cursor_from_c_str("expires");
    s_header_enum_to_str[AWS_HTTP_HEADER_DATE] = aws_byte_cursor_from_c_str("date");
    s_header_enum_to_str[AWS_HTTP_HEADER_LOCATION] = aws_byte_cursor_from_c_str("location");
    s_header_enum_to_str[AWS_HTTP_HEADER_RETRY_AFTER] = aws_byte_cursor_from_c_str("retry-after");
    s_header_enum_to_str[AWS_HTTP_HEADER_VARY] = aws_byte_cursor_from_c_str("vary");
    s_header_enum_to_str[AWS_HTTP_HEADER_WARNING] = aws_byte_cursor_from_c_str("warning");
    s_header_enum_to_str[AWS_HTTP_HEADER_UPGRADE] = aws_byte_cursor_from_c_str("upgrade");
    s_header_enum_to_str[AWS_HTTP_HEADER_KEEP_ALIVE] = aws_byte_cursor_from_c_str("keep-alive");
    s_header_enum_to_str[AWS_HTTP_HEADER_PROXY_CONNECTION] = aws_byte_cursor_from_c_str("proxy-connection");

    s_init_str_to_enum_hash_table(
        &s_header_str_to_enum,
        alloc,
        s_header_enum_to_str,
        AWS_HTTP_HEADER_UNKNOWN + 1,
        AWS_HTTP_HEADER_COUNT,
        true /* ignore case */);

    s_init_str_to_enum_hash_table(
        &s_lowercase_header_str_to_enum,
        alloc,
        s_header_enum_to_str,
        AWS_HTTP_HEADER_UNKNOWN + 1,
        AWS_HTTP_HEADER_COUNT,
        false /* ignore case */);
}

static void s_headers_clean_up(void) {
    aws_hash_table_clean_up(&s_header_str_to_enum);
    aws_hash_table_clean_up(&s_lowercase_header_str_to_enum);
}

enum aws_http_header_name aws_http_str_to_header_name(struct aws_byte_cursor cursor) {
    int header = s_find_in_str_to_enum_hash_table(&s_header_str_to_enum, &cursor);
    if (header >= 0) {
        return (enum aws_http_header_name)header;
    }
    return AWS_HTTP_HEADER_UNKNOWN;
}

enum aws_http_header_name aws_http_lowercase_str_to_header_name(struct aws_byte_cursor cursor) {
    int header = s_find_in_str_to_enum_hash_table(&s_lowercase_header_str_to_enum, &cursor);
    if (header >= 0) {
        return (enum aws_http_header_name)header;
    }
    return AWS_HTTP_HEADER_UNKNOWN;
}

/* STATUS */
const char *aws_http_status_text(int status_code) {
    /**
     * Data from Internet Assigned Numbers Authority (IANA):
     * https://www.iana.org/assignments/http-status-codes/http-status-codes.txt
     */
    switch (status_code) {
        case AWS_HTTP_STATUS_CODE_100_CONTINUE:
            return "Continue";
        case AWS_HTTP_STATUS_CODE_101_SWITCHING_PROTOCOLS:
            return "Switching Protocols";
        case AWS_HTTP_STATUS_CODE_102_PROCESSING:
            return "Processing";
        case AWS_HTTP_STATUS_CODE_103_EARLY_HINTS:
            return "Early Hints";
        case AWS_HTTP_STATUS_CODE_200_OK:
            return "OK";
        case AWS_HTTP_STATUS_CODE_201_CREATED:
            return "Created";
        case AWS_HTTP_STATUS_CODE_202_ACCEPTED:
            return "Accepted";
        case AWS_HTTP_STATUS_CODE_203_NON_AUTHORITATIVE_INFORMATION:
            return "Non-Authoritative Information";
        case AWS_HTTP_STATUS_CODE_204_NO_CONTENT:
            return "No Content";
        case AWS_HTTP_STATUS_CODE_205_RESET_CONTENT:
            return "Reset Content";
        case AWS_HTTP_STATUS_CODE_206_PARTIAL_CONTENT:
            return "Partial Content";
        case AWS_HTTP_STATUS_CODE_207_MULTI_STATUS:
            return "Multi-Status";
        case AWS_HTTP_STATUS_CODE_208_ALREADY_REPORTED:
            return "Already Reported";
        case AWS_HTTP_STATUS_CODE_226_IM_USED:
            return "IM Used";
        case AWS_HTTP_STATUS_CODE_300_MULTIPLE_CHOICES:
            return "Multiple Choices";
        case AWS_HTTP_STATUS_CODE_301_MOVED_PERMANENTLY:
            return "Moved Permanently";
        case AWS_HTTP_STATUS_CODE_302_FOUND:
            return "Found";
        case AWS_HTTP_STATUS_CODE_303_SEE_OTHER:
            return "See Other";
        case AWS_HTTP_STATUS_CODE_304_NOT_MODIFIED:
            return "Not Modified";
        case AWS_HTTP_STATUS_CODE_305_USE_PROXY:
            return "Use Proxy";
        case AWS_HTTP_STATUS_CODE_307_TEMPORARY_REDIRECT:
            return "Temporary Redirect";
        case AWS_HTTP_STATUS_CODE_308_PERMANENT_REDIRECT:
            return "Permanent Redirect";
        case AWS_HTTP_STATUS_CODE_400_BAD_REQUEST:
            return "Bad Request";
        case AWS_HTTP_STATUS_CODE_401_UNAUTHORIZED:
            return "Unauthorized";
        case AWS_HTTP_STATUS_CODE_402_PAYMENT_REQUIRED:
            return "Payment Required";
        case AWS_HTTP_STATUS_CODE_403_FORBIDDEN:
            return "Forbidden";
        case AWS_HTTP_STATUS_CODE_404_NOT_FOUND:
            return "Not Found";
        case AWS_HTTP_STATUS_CODE_405_METHOD_NOT_ALLOWED:
            return "Method Not Allowed";
        case AWS_HTTP_STATUS_CODE_406_NOT_ACCEPTABLE:
            return "Not Acceptable";
        case AWS_HTTP_STATUS_CODE_407_PROXY_AUTHENTICATION_REQUIRED:
            return "Proxy Authentication Required";
        case AWS_HTTP_STATUS_CODE_408_REQUEST_TIMEOUT:
            return "Request Timeout";
        case AWS_HTTP_STATUS_CODE_409_CONFLICT:
            return "Conflict";
        case AWS_HTTP_STATUS_CODE_410_GONE:
            return "Gone";
        case AWS_HTTP_STATUS_CODE_411_LENGTH_REQUIRED:
            return "Length Required";
        case AWS_HTTP_STATUS_CODE_412_PRECONDITION_FAILED:
            return "Precondition Failed";
        case AWS_HTTP_STATUS_CODE_413_REQUEST_ENTITY_TOO_LARGE:
            return "Payload Too Large";
        case AWS_HTTP_STATUS_CODE_414_REQUEST_URI_TOO_LONG:
            return "URI Too Long";
        case AWS_HTTP_STATUS_CODE_415_UNSUPPORTED_MEDIA_TYPE:
            return "Unsupported Media Type";
        case AWS_HTTP_STATUS_CODE_416_REQUESTED_RANGE_NOT_SATISFIABLE:
            return "Range Not Satisfiable";
        case AWS_HTTP_STATUS_CODE_417_EXPECTATION_FAILED:
            return "Expectation Failed";
        case AWS_HTTP_STATUS_CODE_421_MISDIRECTED_REQUEST:
            return "Misdirected Request";
        case AWS_HTTP_STATUS_CODE_422_UNPROCESSABLE_ENTITY:
            return "Unprocessable Entity";
        case AWS_HTTP_STATUS_CODE_423_LOCKED:
            return "Locked";
        case AWS_HTTP_STATUS_CODE_424_FAILED_DEPENDENCY:
            return "Failed Dependency";
        case AWS_HTTP_STATUS_CODE_425_TOO_EARLY:
            return "Too Early";
        case AWS_HTTP_STATUS_CODE_426_UPGRADE_REQUIRED:
            return "Upgrade Required";
        case AWS_HTTP_STATUS_CODE_428_PRECONDITION_REQUIRED:
            return "Precondition Required";
        case AWS_HTTP_STATUS_CODE_429_TOO_MANY_REQUESTS:
            return "Too Many Requests";
        case AWS_HTTP_STATUS_CODE_431_REQUEST_HEADER_FIELDS_TOO_LARGE:
            return "Request Header Fields Too Large";
        case AWS_HTTP_STATUS_CODE_451_UNAVAILABLE_FOR_LEGAL_REASON:
            return "Unavailable For Legal Reasons";
        case AWS_HTTP_STATUS_CODE_500_INTERNAL_SERVER_ERROR:
            return "Internal Server Error";
        case AWS_HTTP_STATUS_CODE_501_NOT_IMPLEMENTED:
            return "Not Implemented";
        case AWS_HTTP_STATUS_CODE_502_BAD_GATEWAY:
            return "Bad Gateway";
        case AWS_HTTP_STATUS_CODE_503_SERVICE_UNAVAILABLE:
            return "Service Unavailable";
        case AWS_HTTP_STATUS_CODE_504_GATEWAY_TIMEOUT:
            return "Gateway Timeout";
        case AWS_HTTP_STATUS_CODE_505_HTTP_VERSION_NOT_SUPPORTED:
            return "HTTP Version Not Supported";
        case AWS_HTTP_STATUS_CODE_506_VARIANT_ALSO_NEGOTIATES:
            return "Variant Also Negotiates";
        case AWS_HTTP_STATUS_CODE_507_INSUFFICIENT_STORAGE:
            return "Insufficient Storage";
        case AWS_HTTP_STATUS_CODE_508_LOOP_DETECTED:
            return "Loop Detected";
        case AWS_HTTP_STATUS_CODE_510_NOT_EXTENDED:
            return "Not Extended";
        case AWS_HTTP_STATUS_CODE_511_NETWORK_AUTHENTICATION_REQUIRED:
            return "Network Authentication Required";
        default:
            return "";
    }
}

static bool s_library_initialized = false;
void aws_http_library_init(struct aws_allocator *alloc) {
    if (s_library_initialized) {
        return;
    }
    s_library_initialized = true;

    aws_io_library_init(alloc);
    aws_compression_library_init(alloc);
    aws_register_error_info(&s_error_list);
    aws_register_log_subject_info_list(&s_log_subject_list);
    s_methods_init(alloc);
    s_headers_init(alloc);
    s_versions_init(alloc);
    aws_hpack_static_table_init(alloc);
}

void aws_http_library_clean_up(void) {
    if (!s_library_initialized) {
        return;
    }
    s_library_initialized = false;

    aws_thread_join_all_managed();
    aws_unregister_error_info(&s_error_list);
    aws_unregister_log_subject_info_list(&s_log_subject_list);
    s_methods_clean_up();
    s_headers_clean_up();
    s_versions_clean_up();
    aws_hpack_static_table_clean_up();
    aws_compression_library_clean_up();
    aws_io_library_clean_up();
}

void aws_http_fatal_assert_library_initialized(void) {
    if (!s_library_initialized) {
        AWS_LOGF_FATAL(
            AWS_LS_HTTP_GENERAL,
            "aws_http_library_init() must be called before using any functionality in aws-c-http.");

        AWS_FATAL_ASSERT(s_library_initialized);
    }
}

const struct aws_byte_cursor aws_http_method_get = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("GET");
const struct aws_byte_cursor aws_http_method_head = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("HEAD");
const struct aws_byte_cursor aws_http_method_post = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("POST");
const struct aws_byte_cursor aws_http_method_put = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("PUT");
const struct aws_byte_cursor aws_http_method_delete = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("DELETE");
const struct aws_byte_cursor aws_http_method_connect = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("CONNECT");
const struct aws_byte_cursor aws_http_method_options = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("OPTIONS");

const struct aws_byte_cursor aws_http_header_method = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":method");
const struct aws_byte_cursor aws_http_header_scheme = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":scheme");
const struct aws_byte_cursor aws_http_header_authority = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":authority");
const struct aws_byte_cursor aws_http_header_path = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":path");
const struct aws_byte_cursor aws_http_header_status = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(":status");

const struct aws_byte_cursor aws_http_scheme_http = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("http");
const struct aws_byte_cursor aws_http_scheme_https = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("https");
