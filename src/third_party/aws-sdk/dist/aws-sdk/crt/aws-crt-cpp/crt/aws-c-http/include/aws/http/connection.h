#ifndef AWS_HTTP_CONNECTION_H
#define AWS_HTTP_CONNECTION_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_client_bootstrap;
struct aws_socket_options;
struct aws_socket_endpoint;
struct aws_tls_connection_options;
struct aws_http2_setting;
struct proxy_env_var_settings;

/**
 * An HTTP connection.
 * This type is used by both server-side and client-side connections.
 * This type is also used by all supported versions of HTTP.
 */
struct aws_http_connection;

/**
 * Invoked when connect completes.
 *
 * If unsuccessful, error_code will be set, connection will be NULL,
 * and the on_shutdown callback will never be invoked.
 *
 * If successful, error_code will be 0 and connection will be valid.
 * The user is now responsible for the connection and must
 * call aws_http_connection_release() when they are done with it.
 *
 * The connection uses one event-loop thread to do all its work.
 * The thread invoking this callback will be the same thread that invokes all
 * future callbacks for this connection and its streams.
 */
typedef void(
    aws_http_on_client_connection_setup_fn)(struct aws_http_connection *connection, int error_code, void *user_data);

/**
 * Invoked when the connection has finished shutting down.
 * Never invoked if on_setup failed.
 * This is always invoked on connection's event-loop thread.
 * Note that the connection is not completely done until on_shutdown has been invoked
 * AND aws_http_connection_release() has been called.
 */
typedef void(
    aws_http_on_client_connection_shutdown_fn)(struct aws_http_connection *connection, int error_code, void *user_data);

/**
 * Invoked when the HTTP/2 settings change is complete.
 * If connection setup successfully this will always be invoked whether settings change successfully or unsuccessfully.
 * If error_code is AWS_ERROR_SUCCESS (0), then the peer has acknowledged the settings and the change has been applied.
 * If error_code is non-zero, then a connection error occurred before the settings could be fully acknowledged and
 * applied. This is always invoked on the connection's event-loop thread.
 */
typedef void(aws_http2_on_change_settings_complete_fn)(
    struct aws_http_connection *http2_connection,
    int error_code,
    void *user_data);

/**
 * Invoked when the HTTP/2 PING completes, whether peer has acknowledged it or not.
 * If error_code is AWS_ERROR_SUCCESS (0), then the peer has acknowledged the PING and round_trip_time_ns will be the
 * round trip time in nano seconds for the connection.
 * If error_code is non-zero, then a connection error occurred before the PING get acknowledgment and round_trip_time_ns
 * will be useless in this case.
 */
typedef void(aws_http2_on_ping_complete_fn)(
    struct aws_http_connection *http2_connection,
    uint64_t round_trip_time_ns,
    int error_code,
    void *user_data);

/**
 * Invoked when an HTTP/2 GOAWAY frame is received from peer.
 * Implies that the peer has initiated shutdown, or encountered a serious error.
 * Once a GOAWAY is received, no further streams may be created on this connection.
 *
 * @param http2_connection This HTTP/2 connection.
 * @param last_stream_id ID of the last locally-initiated stream that peer will
 *      process. Any locally-initiated streams with a higher ID are ignored by
 *      peer, and are safe to retry on another connection.
 * @param http2_error_code The HTTP/2 error code (RFC-7540 section 7) sent by peer.
 *      `enum aws_http2_error_code` lists official codes.
 * @param debug_data The debug data sent by peer. It can be empty. (NOTE: this data is only valid for the lifetime of
 *      the callback. Make a deep copy if you wish to keep it longer.)
 * @param user_data User-data passed to the callback.
 */
typedef void(aws_http2_on_goaway_received_fn)(
    struct aws_http_connection *http2_connection,
    uint32_t last_stream_id,
    uint32_t http2_error_code,
    struct aws_byte_cursor debug_data,
    void *user_data);

/**
 * Invoked when new HTTP/2 settings from peer have been applied.
 * Settings_array is the array of aws_http2_settings that contains all the settings we just changed in the order we
 * applied (the order settings arrived). Num_settings is the number of elements in that array.
 */
typedef void(aws_http2_on_remote_settings_change_fn)(
    struct aws_http_connection *http2_connection,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    void *user_data);

/**
 * Callback invoked on each statistics sample.
 *
 * connection_nonce is unique to each connection for disambiguation of each callback per connection.
 */
typedef void(
    aws_http_statistics_observer_fn)(size_t connection_nonce, const struct aws_array_list *stats_list, void *user_data);

/**
 * Configuration options for connection monitoring
 */
struct aws_http_connection_monitoring_options {

    /**
     * minimum required throughput of the connection.  Throughput is only measured against the interval of time where
     * there is actual io to perform.  Read and write throughput are measured and checked independently of one another.
     */
    uint64_t minimum_throughput_bytes_per_second;

    /*
     * amount of time, in seconds, throughput is allowed to drop below the minimum before the connection is shut down
     * as unhealthy.
     */
    uint32_t allowable_throughput_failure_interval_seconds;

    /**
     * invoked on each statistics publish by the underlying IO channel. Install this callback to receive the statistics
     * for observation. This field is optional.
     */
    aws_http_statistics_observer_fn *statistics_observer_fn;

    /**
     * user_data to be passed to statistics_observer_fn.
     */
    void *statistics_observer_user_data;
};

/**
 * Options specific to HTTP/1.x connections.
 */
struct aws_http1_connection_options {
    /**
     * Optional
     * Capacity in bytes of the HTTP/1 connection's read buffer.
     * The buffer grows if the flow-control window of the incoming HTTP-stream
     * reaches zero. If the buffer reaches capacity, no further socket data is
     * read until the HTTP-stream's window opens again, allowing data to resume flowing.
     *
     * Ignored if `manual_window_management` is false.
     * If zero is specified (the default) then a default capacity is chosen.
     * A capacity that is too small may hinder throughput.
     * A capacity that is too big may waste memory without helping throughput.
     */
    size_t read_buffer_capacity;
};

/**
 * Options specific to HTTP/2 connections.
 */
struct aws_http2_connection_options {
    /**
     * Optional
     * The data of settings to change for initial settings.
     * Note: each setting has its boundary. If settings_array is not set, num_settings has to be 0 to send an empty
     * SETTINGS frame.
     */
    struct aws_http2_setting *initial_settings_array;

    /**
     * Required
     * The num of settings to change (Length of the initial_settings_array).
     */
    size_t num_initial_settings;

    /**
     * Optional.
     * Invoked when the HTTP/2 initial settings change is complete.
     * If failed to setup the connection, this will not be invoked.
     * Otherwise, this will be invoked, whether settings change successfully or unsuccessfully.
     * See `aws_http2_on_change_settings_complete_fn`.
     */
    aws_http2_on_change_settings_complete_fn *on_initial_settings_completed;

    /**
     * Optional
     * The max number of recently-closed streams to remember.
     * Set it to zero to use the default setting, AWS_HTTP2_DEFAULT_MAX_CLOSED_STREAMS
     *
     * If the connection receives a frame for a closed stream,
     * the frame will be ignored or cause a connection error,
     * depending on the frame type and how the stream was closed.
     * Remembering more streams reduces the chances that a late frame causes
     * a connection error, but costs some memory.
     */
    size_t max_closed_streams;

    /**
     * Optional.
     * Invoked when a valid GOAWAY frame received.
     * See `aws_http2_on_goaway_received_fn`.
     */
    aws_http2_on_goaway_received_fn *on_goaway_received;

    /**
     * Optional.
     * Invoked when new settings from peer have been applied.
     * See `aws_http2_on_remote_settings_change_fn`.
     */
    aws_http2_on_remote_settings_change_fn *on_remote_settings_change;

    /**
     * Optional.
     * Set to true to manually manage the flow-control window of whole HTTP/2 connection.
     *
     * If false, the connection will maintain its flow-control windows such that
     * no back-pressure is applied and data arrives as fast as possible.
     *
     * If true, the flow-control window of the whole connection will shrink as body data
     * is received (headers, padding, and other metadata do not affect the window) for every streams
     * created on this connection.
     * The initial connection flow-control window is 65,535.
     * Once the connection's flow-control window reaches to 0, all the streams on the connection stop receiving any
     * further data.
     * The user must call aws_http2_connection_update_window() to increment the connection's
     * window and keep data flowing.
     * Note: the padding of data frame counts to the flow-control window.
     * But, the client will always automatically update the window for padding even for manual window update.
     */
    bool conn_manual_window_management;
};

/**
 * Options for creating an HTTP client connection.
 * Initialize with AWS_HTTP_CLIENT_CONNECTION_OPTIONS_INIT to set default values.
 */
struct aws_http_client_connection_options {
    /**
     * The sizeof() this struct, used for versioning.
     * Set by AWS_HTTP_CLIENT_CONNECTION_OPTIONS_INIT.
     */
    size_t self_size;

    /**
     * Required.
     * Must outlive the connection.
     */
    struct aws_allocator *allocator;

    /**
     * Required.
     * The connection keeps the bootstrap alive via ref-counting.
     */
    struct aws_client_bootstrap *bootstrap;

    /**
     * Required.
     * aws_http_client_connect() makes a copy.
     */
    struct aws_byte_cursor host_name;

    /**
     * Required.
     */
    uint32_t port;

    /**
     * Required.
     * aws_http_client_connect() makes a copy.
     */
    const struct aws_socket_options *socket_options;

    /**
     * Optional.
     * aws_http_client_connect() deep-copies all contents,
     * and keeps `aws_tls_ctx` alive via ref-counting.
     */
    const struct aws_tls_connection_options *tls_options;

    /**
     * Optional
     * Configuration options related to http proxy usage.
     * Relevant fields are copied internally.
     */
    const struct aws_http_proxy_options *proxy_options;

    /*
     * Optional.
     * Configuration for using proxy from environment variable.
     * Only works when proxy_options is not set.
     */
    const struct proxy_env_var_settings *proxy_ev_settings;

    /**
     * Optional
     * Configuration options related to connection health monitoring
     */
    const struct aws_http_connection_monitoring_options *monitoring_options;

    /**
     * Optional (ignored if 0).
     * After a request is fully sent, if the server does not begin responding within N milliseconds,
     * then fail with AWS_ERROR_HTTP_RESPONSE_FIRST_BYTE_TIMEOUT.
     * This can be overridden per-request by aws_http_make_request_options.response_first_byte_timeout_ms.
     * TODO: Only supported in HTTP/1.1 now, support it in HTTP/2
     */
    uint64_t response_first_byte_timeout_ms;

    /**
     * Set to true to manually manage the flow-control window of each stream.
     *
     * If false, the connection will maintain its flow-control windows such that
     * no back-pressure is applied and data arrives as fast as possible.
     *
     * If true, the flow-control window of each stream will shrink as body data
     * is received (headers, padding, and other metadata do not affect the window).
     * `initial_window_size` determines the starting size of each stream's window for HTTP/1 stream, while HTTP/2 stream
     * will use the settings AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE to inform the other side about read back pressure
     *
     * If a stream's flow-control window reaches 0, no further data will be received. The user must call
     * aws_http_stream_update_window() to increment the stream's window and keep data flowing.
     *
     * If a HTTP/2 connection created, it will ONLY control the stream window
     * management. Connection window management is controlled by
     * conn_manual_window_management. Note: the padding of data frame counts to the flow-control window.
     * But, the client will always automatically update the window for padding even for manual window update.
     */
    bool manual_window_management;

    /**
     * The starting size of each HTTP stream's flow-control window for HTTP/1 connection.
     * Required if `manual_window_management` is true,
     * ignored if `manual_window_management` is false.
     *
     * Always ignored when HTTP/2 connection created. The initial window size is controlled by the settings,
     * `AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE`
     */
    size_t initial_window_size;

    /**
     * User data for callbacks
     * Optional.
     */
    void *user_data;

    /**
     * Invoked when connect completes.
     * Required.
     * See `aws_http_on_client_connection_setup_fn`.
     */
    aws_http_on_client_connection_setup_fn *on_setup;

    /**
     * Invoked when the connection has finished shutting down.
     * Never invoked if setup failed.
     * Optional.
     * See `aws_http_on_client_connection_shutdown_fn`.
     */
    aws_http_on_client_connection_shutdown_fn *on_shutdown;

    /**
     * Optional.
     * When true, use prior knowledge to set up an HTTP/2 connection on a cleartext
     * connection.
     * When TLS is set and this is true, the connection will failed to be established,
     * as prior knowledge only works for cleartext TLS.
     * Refer to RFC7540 3.4
     */
    bool prior_knowledge_http2;

    /**
     * Optional.
     * Pointer to the hash map containing the ALPN string to protocol to use.
     * Hash from `struct aws_string *` to `enum aws_http_version`.
     * If not set, only the predefined string `h2` and `http/1.1` will be recognized. Other negotiated ALPN string will
     * result in a HTTP1/1 connection
     * Note: Connection will keep a deep copy of the table and the strings.
     */
    struct aws_hash_table *alpn_string_map;

    /**
     * Options specific to HTTP/1.x connections.
     * Optional.
     * Ignored if connection is not HTTP/1.x.
     * If connection is HTTP/1.x and options were not specified, default values are used.
     */
    const struct aws_http1_connection_options *http1_options;

    /**
     * Options specific to HTTP/2 connections.
     * Optional.
     * Ignored if connection is not HTTP/2.
     * If connection is HTTP/2 and options were not specified, default values are used.
     */
    const struct aws_http2_connection_options *http2_options;

    /**
     * Optional.
     * Requests the channel/connection be bound to a specific event loop rather than chosen sequentially from the
     * event loop group associated with the client bootstrap.
     */
    struct aws_event_loop *requested_event_loop;

    /**
     * Optional
     * Host resolution override that allows the user to override DNS behavior for this particular connection.
     */
    const struct aws_host_resolution_config *host_resolution_config;
};

/* Predefined settings identifiers (RFC-7540 6.5.2) */
enum aws_http2_settings_id {
    AWS_HTTP2_SETTINGS_BEGIN_RANGE = 0x1, /* Beginning of known values */
    AWS_HTTP2_SETTINGS_HEADER_TABLE_SIZE = 0x1,
    AWS_HTTP2_SETTINGS_ENABLE_PUSH = 0x2,
    AWS_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE = 0x5,
    AWS_HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE = 0x6,
    AWS_HTTP2_SETTINGS_END_RANGE, /* End of known values */
};

/* A HTTP/2 setting and its value, used in SETTINGS frame */
struct aws_http2_setting {
    enum aws_http2_settings_id id;
    uint32_t value;
};

/**
 * HTTP/2: Default value for max closed streams we will keep in memory.
 */
#define AWS_HTTP2_DEFAULT_MAX_CLOSED_STREAMS (32)

/**
 * HTTP/2: The size of payload for HTTP/2 PING frame.
 */
#define AWS_HTTP2_PING_DATA_SIZE (8)

/**
 * HTTP/2: The number of known settings.
 */
#define AWS_HTTP2_SETTINGS_COUNT (6)

/**
 * Initializes aws_http_client_connection_options with default values.
 */
#define AWS_HTTP_CLIENT_CONNECTION_OPTIONS_INIT                                                                        \
    {                                                                                                                  \
        .self_size = sizeof(struct aws_http_client_connection_options),                                                \
        .initial_window_size = SIZE_MAX,                                                                               \
    }

AWS_EXTERN_C_BEGIN

/**
 * Asynchronously establish a client connection.
 * The on_setup callback is invoked when the operation has created a connection or failed.
 */
AWS_HTTP_API
int aws_http_client_connect(const struct aws_http_client_connection_options *options);

/**
 * Users must release the connection when they are done with it.
 * The connection's memory cannot be reclaimed until this is done.
 * If the connection was not already shutting down, it will be shut down.
 *
 * Users should always wait for the on_shutdown() callback to be called before releasing any data passed to the
 * http_connection (Eg aws_tls_connection_options, aws_socket_options) otherwise there will be race conditions between
 * http_connection shutdown tasks and memory release tasks, causing Segfaults.
 */
AWS_HTTP_API
void aws_http_connection_release(struct aws_http_connection *connection);

/**
 * Begin shutdown sequence of the connection if it hasn't already started. This will schedule shutdown tasks on the
 * EventLoop that may send HTTP/TLS/TCP shutdown messages to peers if necessary, and will eventually cause internal
 * connection memory to stop being accessed and on_shutdown() callback to be called.
 *
 * It's safe to call this function regardless of the connection state as long as you hold a reference to the connection.
 */
AWS_HTTP_API
void aws_http_connection_close(struct aws_http_connection *connection);

/**
 * Stop accepting new requests for the connection. It will NOT start the shutdown process for the connection. The
 * requests that are already open can still wait to be completed, but new requests will fail to be created,
 */
AWS_HTTP_API
void aws_http_connection_stop_new_requests(struct aws_http_connection *connection);

/**
 * Returns true unless the connection is closed or closing.
 */
AWS_HTTP_API
bool aws_http_connection_is_open(const struct aws_http_connection *connection);

/**
 * Return whether the connection can make a new requests.
 * If false, then a new connection must be established to make further requests.
 */
AWS_HTTP_API
bool aws_http_connection_new_requests_allowed(const struct aws_http_connection *connection);

/**
 * Returns true if this is a client connection.
 */
AWS_HTTP_API
bool aws_http_connection_is_client(const struct aws_http_connection *connection);

AWS_HTTP_API
enum aws_http_version aws_http_connection_get_version(const struct aws_http_connection *connection);

/**
 * Returns the channel hosting the HTTP connection.
 * Do not expose this function to language bindings.
 */
AWS_HTTP_API
struct aws_channel *aws_http_connection_get_channel(struct aws_http_connection *connection);

/**
 * Returns the remote endpoint of the HTTP connection.
 */
AWS_HTTP_API
const struct aws_socket_endpoint *aws_http_connection_get_remote_endpoint(const struct aws_http_connection *connection);

/**
 * Initialize an map copied from the *src map, which maps `struct aws_string *` to `enum aws_http_version`.
 */
AWS_HTTP_API
int aws_http_alpn_map_init_copy(
    struct aws_allocator *allocator,
    struct aws_hash_table *dest,
    struct aws_hash_table *src);

/**
 * Initialize an empty hash-table that maps `struct aws_string *` to `enum aws_http_version`.
 * This map can used in aws_http_client_connections_options.alpn_string_map.
 */
AWS_HTTP_API
int aws_http_alpn_map_init(struct aws_allocator *allocator, struct aws_hash_table *map);

/**
 * Checks http proxy options for correctness
 */
AWS_HTTP_API
int aws_http_options_validate_proxy_configuration(const struct aws_http_client_connection_options *options);

/**
 * Send a SETTINGS frame (HTTP/2 only).
 * SETTINGS will be applied locally when SETTINGS ACK is received from peer.
 *
 * @param http2_connection HTTP/2 connection.
 * @param settings_array The array of settings to change. Note: each setting has its boundary.
 * @param num_settings The num of settings to change in settings_array.
 * @param on_completed Optional callback, see `aws_http2_on_change_settings_complete_fn`.
 * @param user_data User-data pass to on_completed callback.
 */
AWS_HTTP_API
int aws_http2_connection_change_settings(
    struct aws_http_connection *http2_connection,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    aws_http2_on_change_settings_complete_fn *on_completed,
    void *user_data);

/**
 * Send a PING frame (HTTP/2 only).
 * Round-trip-time is calculated when PING ACK is received from peer.
 *
 * @param http2_connection HTTP/2 connection.
 * @param optional_opaque_data Optional payload for PING frame.
 *      Must be NULL, or exactly 8 bytes (AWS_HTTP2_PING_DATA_SIZE).
 *      If NULL, the 8 byte payload will be all zeroes.
 * @param on_completed Optional callback, invoked when PING ACK is received from peer,
 *      or when a connection error prevents the PING ACK from being received.
 *      Callback always fires on the connection's event-loop thread.
 * @param user_data User-data pass to on_completed callback.
 */
AWS_HTTP_API
int aws_http2_connection_ping(
    struct aws_http_connection *http2_connection,
    const struct aws_byte_cursor *optional_opaque_data,
    aws_http2_on_ping_complete_fn *on_completed,
    void *user_data);

/**
 * Get the local settings we are using to affect the decoding.
 *
 * @param http2_connection HTTP/2 connection.
 * @param out_settings fixed size array of aws_http2_setting gets set to the local settings
 */
AWS_HTTP_API
void aws_http2_connection_get_local_settings(
    const struct aws_http_connection *http2_connection,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]);

/**
 * Get the settings received from remote peer, which we are using to restricts the message to send.
 *
 * @param http2_connection HTTP/2 connection.
 * @param out_settings fixed size array of aws_http2_setting gets set to the remote settings
 */
AWS_HTTP_API
void aws_http2_connection_get_remote_settings(
    const struct aws_http_connection *http2_connection,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]);

/**
 * Send a custom GOAWAY frame (HTTP/2 only).
 *
 * Note that the connection automatically attempts to send a GOAWAY during
 * shutdown (unless a GOAWAY with a valid Last-Stream-ID has already been sent).
 *
 * This call can be used to gracefully warn the peer of an impending shutdown
 * (http2_error=0, allow_more_streams=true), or to customize the final GOAWAY
 * frame that is sent by this connection.
 *
 * The other end may not receive the goaway, if the connection already closed.
 *
 * @param http2_connection HTTP/2 connection.
 * @param http2_error The HTTP/2 error code (RFC-7540 section 7) to send.
 *      `enum aws_http2_error_code` lists official codes.
 * @param allow_more_streams If true, new peer-initiated streams will continue
 *      to be acknowledged and the GOAWAY's Last-Stream-ID will be set to a max value.
 *      If false, new peer-initiated streams will be ignored and the GOAWAY's
 *      Last-Stream-ID will be set to the latest acknowledged stream.
 * @param optional_debug_data Optional debug data to send. Size must not exceed 16KB.
 */

AWS_HTTP_API
void aws_http2_connection_send_goaway(
    struct aws_http_connection *http2_connection,
    uint32_t http2_error,
    bool allow_more_streams,
    const struct aws_byte_cursor *optional_debug_data);

/**
 * Get data about the latest GOAWAY frame sent to peer (HTTP/2 only).
 * If no GOAWAY has been sent, AWS_ERROR_HTTP_DATA_NOT_AVAILABLE will be raised.
 * Note that GOAWAY frames are typically sent automatically by the connection
 * during shutdown.
 *
 * @param http2_connection HTTP/2 connection.
 * @param out_http2_error Gets set to HTTP/2 error code sent in most recent GOAWAY.
 * @param out_last_stream_id Gets set to Last-Stream-ID sent in most recent GOAWAY.
 */
AWS_HTTP_API
int aws_http2_connection_get_sent_goaway(
    struct aws_http_connection *http2_connection,
    uint32_t *out_http2_error,
    uint32_t *out_last_stream_id);

/**
 * Get data about the latest GOAWAY frame received from peer (HTTP/2 only).
 * If no GOAWAY has been received, or the GOAWAY payload is still in transmitting,
 * AWS_ERROR_HTTP_DATA_NOT_AVAILABLE will be raised.
 *
 * @param http2_connection HTTP/2 connection.
 * @param out_http2_error Gets set to HTTP/2 error code received in most recent GOAWAY.
 * @param out_last_stream_id Gets set to Last-Stream-ID received in most recent GOAWAY.
 */
AWS_HTTP_API
int aws_http2_connection_get_received_goaway(
    struct aws_http_connection *http2_connection,
    uint32_t *out_http2_error,
    uint32_t *out_last_stream_id);

/**
 * Increment the connection's flow-control window to keep data flowing (HTTP/2 only).
 *
 * If the connection was created with `conn_manual_window_management` set true,
 * the flow-control window of the connection will shrink as body data is received for all the streams created on it.
 * (headers, padding, and other metadata do not affect the window).
 * The initial connection flow-control window is 65,535.
 * Once the connection's flow-control window reaches to 0, all the streams on the connection stop receiving any further
 * data.
 *
 * If `conn_manual_window_management` is false, this call will have no effect.
 * The connection maintains its flow-control windows such that
 * no back-pressure is applied and data arrives as fast as possible.
 *
 * If you are not connected, this call will have no effect.
 *
 * Crashes when the connection is not http2 connection.
 * The limit of the Maximum Size is 2**31 - 1. If the increment size cause the connection flow window exceeds the
 * Maximum size, this call will result in the connection lost.
 *
 * @param http2_connection HTTP/2 connection.
 * @param increment_size The size to increment for the connection's flow control window
 */
AWS_HTTP_API
void aws_http2_connection_update_window(struct aws_http_connection *http2_connection, uint32_t increment_size);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_HTTP_CONNECTION_H */
