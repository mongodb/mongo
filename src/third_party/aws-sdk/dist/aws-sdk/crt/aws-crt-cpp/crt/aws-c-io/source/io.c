/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/io.h>

#include <aws/io/logging.h>

#include <aws/cal/cal.h>
#include <aws/io/private/tracing.h>

#define AWS_DEFINE_ERROR_INFO_IO(CODE, STR) [(CODE) - 0x0400] = AWS_DEFINE_ERROR_INFO(CODE, STR, "aws-c-io")

#define AWS_DEFINE_ERROR_PKCS11_CKR(CKR)                                                                               \
    AWS_DEFINE_ERROR_INFO_IO(                                                                                          \
        AWS_ERROR_PKCS11_##CKR, "A PKCS#11 (Cryptoki) library function failed with return value " #CKR)

/* clang-format off */
static struct aws_error_info s_errors[] = {
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_CHANNEL_ERROR_ERROR_CANT_ACCEPT_INPUT,
        "Channel cannot accept input"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_CHANNEL_UNKNOWN_MESSAGE_TYPE,
        "Channel unknown message type"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_CHANNEL_READ_WOULD_EXCEED_WINDOW,
        "A channel handler attempted to propagate a read larger than the upstream window"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_EVENT_LOOP_ALREADY_ASSIGNED,
        "An attempt was made to assign an io handle to an event loop, but the handle was already assigned."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_EVENT_LOOP_SHUTDOWN,
        "Event loop has shutdown and a resource was still using it, the resource has been removed from the loop."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_ERROR_NEGOTIATION_FAILURE,
        "TLS (SSL) negotiation failed"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_ERROR_NOT_NEGOTIATED,
        "Attempt to read/write, but TLS (SSL) hasn't been negotiated"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_ERROR_WRITE_FAILURE,
        "Failed to write to TLS handler"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_ERROR_ALERT_RECEIVED,
        "Fatal TLS Alert was received"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_CTX_ERROR,
        "Failed to create tls context"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_VERSION_UNSUPPORTED,
        "A TLS version was specified that is currently not supported. Consider using AWS_IO_TLS_VER_SYS_DEFAULTS, "
        " and when this lib or the operating system is updated, it will automatically be used."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_CIPHER_PREF_UNSUPPORTED,
        "A TLS Cipher Preference was specified that is currently not supported by the current platform. Consider "
        " using AWS_IO_TLS_CIPHER_SYSTEM_DEFAULT, and when this lib or the operating system is updated, it will "
        "automatically be used."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_MISSING_ALPN_MESSAGE,
        "An ALPN message was expected but not received"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_UNHANDLED_ALPN_PROTOCOL_MESSAGE,
        "An ALPN message was received but a handler was not created by the user"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_FILE_VALIDATION_FAILURE,
        "A file was read and the input did not match the expected value"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_IO_EVENT_LOOP_THREAD_ONLY,
        "Attempt to perform operation that must be run inside the event loop thread"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_IO_ALREADY_SUBSCRIBED,
        "Already subscribed to receive events"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_IO_NOT_SUBSCRIBED,
        "Not subscribed to receive events"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_IO_OPERATION_CANCELLED,
        "Operation cancelled before it could complete"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_READ_WOULD_BLOCK,
        "Read operation would block, try again later"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_BROKEN_PIPE,
        "Attempt to read or write to io handle that has already been closed."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_UNSUPPORTED_ADDRESS_FAMILY,
        "Socket, unsupported address family."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_INVALID_OPERATION_FOR_TYPE,
        "Invalid socket operation for socket type."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_CONNECTION_REFUSED,
        "socket connection refused."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_TIMEOUT,
        "socket operation timed out."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_NO_ROUTE_TO_HOST,
        "socket connect failure, no route to host."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_NETWORK_DOWN,
        "network is down."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_CLOSED,
        "socket is closed."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_NOT_CONNECTED,
        "socket not connected."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_INVALID_OPTIONS,
        "Invalid socket options."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_ADDRESS_IN_USE,
        "Socket address already in use."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_INVALID_ADDRESS,
        "Invalid socket address."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE,
        "Illegal operation for socket state."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SOCKET_CONNECT_ABORTED,
        "Incoming connection was aborted."),
    AWS_DEFINE_ERROR_INFO_IO (
        AWS_IO_DNS_QUERY_FAILED,
        "A query to dns failed to resolve."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_DNS_INVALID_NAME,
        "Host name was invalid for dns resolution."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_DNS_NO_ADDRESS_FOR_HOST,
        "No address was found for the supplied host name."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_DNS_HOST_REMOVED_FROM_CACHE,
        "The entries for host name were removed from the local dns cache."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_STREAM_INVALID_SEEK_POSITION,
        "The seek position was outside of a stream's bounds"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_STREAM_READ_FAILED,
        "Stream failed to read from the underlying io source"),
    AWS_DEFINE_ERROR_INFO_IO(
        DEPRECATED_AWS_IO_INVALID_FILE_HANDLE,
        "Operation failed because the file handle was invalid"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SHARED_LIBRARY_LOAD_FAILURE,
        "System call error during attempt to load shared library"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_SHARED_LIBRARY_FIND_SYMBOL_FAILURE,
        "System call error during attempt to find shared library symbol"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_NEGOTIATION_TIMEOUT,
        "Channel shutdown due to tls negotiation timeout"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_ALERT_NOT_GRACEFUL,
       "Channel shutdown due to tls alert. The alert was not for a graceful shutdown."),
    AWS_DEFINE_ERROR_INFO_IO(
       AWS_IO_MAX_RETRIES_EXCEEDED,
       "Retry cannot be attempted because the maximum number of retries has been exceeded."),
    AWS_DEFINE_ERROR_INFO_IO(
       AWS_IO_RETRY_PERMISSION_DENIED,
       "Retry cannot be attempted because the retry strategy has prevented the operation."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_DIGEST_ALGORITHM_UNSUPPORTED,
        "TLS digest was created with an unsupported algorithm"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_SIGNATURE_ALGORITHM_UNSUPPORTED,
        "TLS signature algorithm is currently unsupported."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_PKCS11_VERSION_UNSUPPORTED,
        "The PKCS#11 library uses an unsupported API version."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_PKCS11_TOKEN_NOT_FOUND,
        "Could not pick PKCS#11 token matching search criteria (none found, or multiple found)"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_PKCS11_KEY_NOT_FOUND,
        "Could not pick PKCS#11 key matching search criteria (none found, or multiple found)"),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_PKCS11_KEY_TYPE_UNSUPPORTED,
        "PKCS#11 key type not supported"),
    AWS_DEFINE_ERROR_INFO_IO(
       AWS_ERROR_PKCS11_UNKNOWN_CRYPTOKI_RETURN_VALUE,
       "A PKCS#11 (Cryptoki) library function failed with an unknown return value (CKR_). See log for more details."),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_CANCEL),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_HOST_MEMORY),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SLOT_ID_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_GENERAL_ERROR),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_FUNCTION_FAILED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_ARGUMENTS_BAD),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_NO_EVENT),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_NEED_TO_CREATE_THREADS),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_CANT_LOCK),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_ATTRIBUTE_READ_ONLY),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_ATTRIBUTE_SENSITIVE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_ATTRIBUTE_TYPE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_ATTRIBUTE_VALUE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_ACTION_PROHIBITED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_DATA_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_DATA_LEN_RANGE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_DEVICE_ERROR),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_DEVICE_MEMORY),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_DEVICE_REMOVED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_ENCRYPTED_DATA_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_ENCRYPTED_DATA_LEN_RANGE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_FUNCTION_CANCELED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_FUNCTION_NOT_PARALLEL),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_FUNCTION_NOT_SUPPORTED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_HANDLE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_SIZE_RANGE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_TYPE_INCONSISTENT),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_NOT_NEEDED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_CHANGED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_NEEDED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_INDIGESTIBLE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_FUNCTION_NOT_PERMITTED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_NOT_WRAPPABLE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_KEY_UNEXTRACTABLE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_MECHANISM_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_MECHANISM_PARAM_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_OBJECT_HANDLE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_OPERATION_ACTIVE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_OPERATION_NOT_INITIALIZED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_PIN_INCORRECT),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_PIN_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_PIN_LEN_RANGE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_PIN_EXPIRED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_PIN_LOCKED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SESSION_CLOSED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SESSION_COUNT),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SESSION_HANDLE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SESSION_PARALLEL_NOT_SUPPORTED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SESSION_READ_ONLY),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SESSION_EXISTS),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SESSION_READ_ONLY_EXISTS),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SESSION_READ_WRITE_SO_EXISTS),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SIGNATURE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SIGNATURE_LEN_RANGE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_TEMPLATE_INCOMPLETE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_TEMPLATE_INCONSISTENT),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_TOKEN_NOT_PRESENT),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_TOKEN_NOT_RECOGNIZED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_TOKEN_WRITE_PROTECTED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_UNWRAPPING_KEY_HANDLE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_UNWRAPPING_KEY_SIZE_RANGE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_UNWRAPPING_KEY_TYPE_INCONSISTENT),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_USER_ALREADY_LOGGED_IN),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_USER_NOT_LOGGED_IN),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_USER_PIN_NOT_INITIALIZED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_USER_TYPE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_USER_ANOTHER_ALREADY_LOGGED_IN),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_USER_TOO_MANY_TYPES),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_WRAPPED_KEY_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_WRAPPED_KEY_LEN_RANGE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_WRAPPING_KEY_HANDLE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_WRAPPING_KEY_SIZE_RANGE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_WRAPPING_KEY_TYPE_INCONSISTENT),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_RANDOM_SEED_NOT_SUPPORTED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_RANDOM_NO_RNG),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_DOMAIN_PARAMS_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_CURVE_NOT_SUPPORTED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_BUFFER_TOO_SMALL),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_SAVED_STATE_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_INFORMATION_SENSITIVE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_STATE_UNSAVEABLE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_CRYPTOKI_NOT_INITIALIZED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_CRYPTOKI_ALREADY_INITIALIZED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_MUTEX_BAD),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_MUTEX_NOT_LOCKED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_NEW_PIN_MODE),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_NEXT_OTP),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_EXCEEDED_MAX_ITERATIONS),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_FIPS_SELF_TEST_FAILED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_LIBRARY_LOAD_FAILED),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_PIN_TOO_WEAK),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_PUBLIC_KEY_INVALID),
    AWS_DEFINE_ERROR_PKCS11_CKR(CKR_FUNCTION_REJECTED),

    AWS_DEFINE_ERROR_INFO_IO(
        AWS_ERROR_IO_PINNED_EVENT_LOOP_MISMATCH,
        "A connection was requested on an event loop that is not associated with the client bootstrap's event loop group."),

    AWS_DEFINE_ERROR_INFO_IO(
       AWS_ERROR_PKCS11_ENCODING_ERROR,
       "A PKCS#11 (Cryptoki) library function was unable to ASN.1 (DER) encode a data structure. See log for more details."),
    AWS_DEFINE_ERROR_INFO_IO(
       AWS_IO_TLS_ERROR_DEFAULT_TRUST_STORE_NOT_FOUND,
        "Default TLS trust store not found on this system."
        " Trusted CA certificates must be installed,"
        " or \"override default trust store\" must be used while creating the TLS context."),

    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_STREAM_SEEK_FAILED,
        "Stream failed to seek from the underlying I/O source."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_STREAM_GET_LENGTH_FAILED,
        "Stream failed to get length from the underlying I/O source."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_STREAM_SEEK_UNSUPPORTED,
        "Seek is not supported in the underlying I/O source."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_STREAM_GET_LENGTH_UNSUPPORTED,
        "Get length is not supported in the underlying I/O source."),
    AWS_DEFINE_ERROR_INFO_IO(
        AWS_IO_TLS_ERROR_READ_FAILURE,
        "Failure during TLS read."),
    AWS_DEFINE_ERROR_INFO_IO(AWS_ERROR_PEM_MALFORMED, "Malformed PEM object encountered."),

};
/* clang-format on */

static struct aws_error_info_list s_list = {
    .error_list = s_errors,
    .count = sizeof(s_errors) / sizeof(struct aws_error_info),
};

static struct aws_log_subject_info s_io_log_subject_infos[] = {
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_IO_GENERAL,
        "aws-c-io",
        "Subject for IO logging that doesn't belong to any particular category"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_EVENT_LOOP, "event-loop", "Subject for Event-loop specific logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_SOCKET, "socket", "Subject for Socket specific logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_SOCKET_HANDLER, "socket-handler", "Subject for a socket channel handler."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_TLS, "tls-handler", "Subject for TLS-related logging"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_ALPN, "alpn", "Subject for ALPN-related logging"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_DNS, "dns", "Subject for DNS-related logging"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_PKI, "pki-utils", "Subject for Pki utilities."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_CHANNEL, "channel", "Subject for Channels"),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_IO_CHANNEL_BOOTSTRAP,
        "channel-bootstrap",
        "Subject for channel bootstrap (client and server modes)"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_FILE_UTILS, "file-utils", "Subject for file operations"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_SHARED_LIBRARY, "shared-library", "Subject for shared library operations"),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_IO_EXPONENTIAL_BACKOFF_RETRY_STRATEGY,
        "exp-backoff-strategy",
        "Subject for exponential backoff retry strategy"),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_IO_STANDARD_RETRY_STRATEGY,
        "standard-retry-strategy",
        "Subject for standard retry strategy"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_PKCS11, "pkcs11", "Subject for PKCS#11 library operations"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_IO_PEM, "pem", "Subject for pem operations")};

static struct aws_log_subject_info_list s_io_log_subject_list = {
    .subject_list = s_io_log_subject_infos,
    .count = AWS_ARRAY_SIZE(s_io_log_subject_infos),
};

static bool s_io_library_initialized = false;

void aws_tls_init_static_state(struct aws_allocator *alloc);
void aws_tls_clean_up_static_state(void);

void aws_io_library_init(struct aws_allocator *allocator) {
    if (!s_io_library_initialized) {
        s_io_library_initialized = true;
        aws_common_library_init(allocator);
        aws_cal_library_init(allocator);
        aws_register_error_info(&s_list);
        aws_register_log_subject_info_list(&s_io_log_subject_list);
        aws_tls_init_static_state(allocator);
        aws_io_tracing_init();
    }
}

void aws_io_library_clean_up(void) {
    if (s_io_library_initialized) {
        s_io_library_initialized = false;
        aws_thread_join_all_managed();
        aws_tls_clean_up_static_state();
        aws_unregister_error_info(&s_list);
        aws_unregister_log_subject_info_list(&s_io_log_subject_list);
        aws_cal_library_clean_up();
        aws_common_library_clean_up();
    }
}

void aws_io_fatal_assert_library_initialized(void) {
    if (!s_io_library_initialized) {
        AWS_LOGF_FATAL(
            AWS_LS_IO_GENERAL, "aws_io_library_init() must be called before using any functionality in aws-c-io.");

        AWS_FATAL_ASSERT(s_io_library_initialized);
    }
}
