#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/connection.h>
#include <aws/http/proxy.h>
#include <aws/http/request_response.h>

#include <aws/crt/Types.h>
#include <aws/crt/io/Bootstrap.h>
#include <aws/crt/io/SocketOptions.h>
#include <aws/crt/io/TlsOptions.h>

#include <functional>
#include <memory>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            class ClientBootstrap;
        }

        namespace Http
        {
            class HttpClientConnection;
            class HttpStream;
            class HttpClientStream;
            class HttpRequest;
            class HttpProxyStrategy;
            using HttpHeader = aws_http_header;

            /**
             * Invoked upon connection setup, whether it was successful or not. If the connection was
             * successfully established, `connection` will be valid and errorCode will be AWS_ERROR_SUCCESS.
             * Upon an error, `connection` will not be valid, and errorCode will contain the cause of the connection
             * failure.
             */
            using OnConnectionSetup =
                std::function<void(const std::shared_ptr<HttpClientConnection> &connection, int errorCode)>;

            /**
             * Invoked upon connection shutdown. `connection` will always be a valid pointer. `errorCode` will specify
             * shutdown reason. A graceful connection close will set `errorCode` to AWS_ERROR_SUCCESS.
             * Internally, the connection pointer will be unreferenced immediately after this call; if you took a
             * reference to it in OnConnectionSetup(), you'll need to release your reference before the underlying
             * memory is released. If you never took a reference to it, the resources for the connection will be
             * immediately released after completion of this callback.
             */
            using OnConnectionShutdown = std::function<void(HttpClientConnection &connection, int errorCode)>;

            /**
             * Called as headers are received from the peer. `headersArray` will contain the header value
             * read from the wire. The number of entries in `headersArray` are specified in `headersCount`.
             *
             * Keep in mind that this function will likely be called multiple times until all headers are received.
             *
             * On HttpStream, this function must be set.
             */
            using OnIncomingHeaders = std::function<void(
                HttpStream &stream,
                enum aws_http_header_block headerBlock,
                const HttpHeader *headersArray,
                std::size_t headersCount)>;

            /**
             * Invoked when the headers portion of the message has been completely received. `hasBody` will indicate
             * if there is an incoming body.
             *
             * On HttpStream, this function can be empty.
             */
            using OnIncomingHeadersBlockDone =
                std::function<void(HttpStream &stream, enum aws_http_header_block block)>;

            /**
             * Invoked as chunks of the body are read. `data` contains the data read from the wire. If chunked encoding
             * was used, it will already be decoded (TBD).
             *
             * On HttpStream, this function can be empty if you are not expecting a body (e.g. a HEAD request).
             */
            using OnIncomingBody = std::function<void(HttpStream &stream, const ByteCursor &data)>;

            /**
             * Invoked upon completion of the stream. This means the request has been sent and a completed response
             * has been received (in client mode), or the request has been received and the response has been completed.
             *
             * In H2, this will mean RST_STREAM state has been reached for the stream.
             *
             * On HttpStream, this function must be set.
             */
            using OnStreamComplete = std::function<void(HttpStream &stream, int errorCode)>;

            /**
             * POD structure used for setting up an Http Request
             */
            struct AWS_CRT_CPP_API HttpRequestOptions
            {
                /**
                 * The actual http request
                 */
                HttpRequest *request;

                /**
                 * See `OnIncomingHeaders` for more info. This value must be set.
                 */
                OnIncomingHeaders onIncomingHeaders;
                OnIncomingHeadersBlockDone onIncomingHeadersBlockDone;

                /**
                 * See `OnIncomingBody` for more info. This value can be empty if you will not be receiving a body.
                 */
                OnIncomingBody onIncomingBody;

                /**
                 * See `OnStreamComplete` for more info. This value can be empty.
                 */
                OnStreamComplete onStreamComplete;
            };

            /**
             * Represents a single http message exchange (request/response) or in H2, it can also represent
             * a PUSH_PROMISE followed by the accompanying Response.
             */
            class AWS_CRT_CPP_API HttpStream : public std::enable_shared_from_this<HttpStream>
            {
              public:
                virtual ~HttpStream();
                HttpStream(const HttpStream &) = delete;
                HttpStream(HttpStream &&) = delete;
                HttpStream &operator=(const HttpStream &) = delete;
                HttpStream &operator=(HttpStream &&) = delete;

                /**
                 * Get the underlying connection for the stream.
                 */
                HttpClientConnection &GetConnection() const noexcept;

                /**
                 * @return request's Http Response Code.  Requires response headers to have been processed first. *
                 */
                virtual int GetResponseStatusCode() const noexcept = 0;

                /**
                 * Updates the read window on the connection. In Http 1.1 this relieves TCP back pressure, in H2
                 * this will trigger two WINDOW_UPDATE frames, one for the connection and one for the stream.
                 *
                 * You do not need to call this unless you utilized the `outWindowUpdateSize` in `OnIncomingBody`.
                 * See `OnIncomingBody` for more information.
                 *
                 * `incrementSize` is the amount to update the read window by.
                 */
                void UpdateWindow(std::size_t incrementSize) noexcept;

              protected:
                aws_http_stream *m_stream;
                std::shared_ptr<HttpClientConnection> m_connection;
                HttpStream(const std::shared_ptr<HttpClientConnection> &connection) noexcept;

              private:
                OnIncomingHeaders m_onIncomingHeaders;
                OnIncomingHeadersBlockDone m_onIncomingHeadersBlockDone;
                OnIncomingBody m_onIncomingBody;
                OnStreamComplete m_onStreamComplete;

                static int s_onIncomingHeaders(
                    struct aws_http_stream *stream,
                    enum aws_http_header_block headerBlock,
                    const struct aws_http_header *headerArray,
                    size_t numHeaders,
                    void *userData) noexcept;
                static int s_onIncomingHeaderBlockDone(
                    struct aws_http_stream *stream,
                    enum aws_http_header_block headerBlock,
                    void *userData) noexcept;
                static int s_onIncomingBody(
                    struct aws_http_stream *stream,
                    const struct aws_byte_cursor *data,
                    void *userData) noexcept;
                static void s_onStreamComplete(struct aws_http_stream *stream, int errorCode, void *userData) noexcept;

                friend class HttpClientConnection;
            };

            struct ClientStreamCallbackData
            {
                ClientStreamCallbackData() : allocator(nullptr), stream(nullptr) {}
                Allocator *allocator;
                std::shared_ptr<HttpStream> stream;
            };

            /**
             * Subclass that represents an http client's view of an HttpStream.
             */
            class AWS_CRT_CPP_API HttpClientStream final : public HttpStream
            {
              public:
                ~HttpClientStream();
                HttpClientStream(const HttpClientStream &) = delete;
                HttpClientStream(HttpClientStream &&) = delete;
                HttpClientStream &operator=(const HttpClientStream &) = delete;
                HttpClientStream &operator=(HttpClientStream &&) = delete;

                /**
                 * If this stream was initiated as a request, assuming the headers of the response has been
                 * received, this value contains the Http Response Code.                 *
                 */
                virtual int GetResponseStatusCode() const noexcept override;

                /**
                 * Activates the request's outgoing stream processing.
                 *
                 * Returns true on success, false otherwise.
                 */
                bool Activate() noexcept;

              private:
                HttpClientStream(const std::shared_ptr<HttpClientConnection> &connection) noexcept;

                ClientStreamCallbackData m_callbackData;
                friend class HttpClientConnection;
            };

            /**
             * @deprecated enum that designates what kind of authentication, if any, to use when connecting to a
             * proxy server.
             *
             * Here for backwards compatibility.  Has been superceded by proxy strategies.
             */
            enum class AwsHttpProxyAuthenticationType
            {
                None,
                Basic,
            };

            /**
             * Mirror of aws_http_proxy_connection_type enum. Indicates the basic http proxy behavior of the
             * proxy we're connecting to.
             */
            enum class AwsHttpProxyConnectionType
            {
                /**
                 * Deprecated, but 0-valued for backwards compatibility
                 *
                 * If tls options are provided (for the main connection) then treat the proxy as a tunneling proxy
                 * If tls options are not provided (for the main connection), then treat the proxy as a forwarding
                 * proxy
                 */
                Legacy = AWS_HPCT_HTTP_LEGACY,

                /**
                 * Use the proxy to forward http requests.  Attempting to use both this mode and TLS to the destination
                 * is a configuration error.
                 */
                Forwarding = AWS_HPCT_HTTP_FORWARD,

                /**
                 * Use the proxy to establish an http connection via a CONNECT request to the proxy.  Works for both
                 * plaintext and tls connections.
                 */
                Tunneling = AWS_HPCT_HTTP_TUNNEL,
            };

            /**
             * Configuration structure that holds all proxy-related http connection options
             */
            class AWS_CRT_CPP_API HttpClientConnectionProxyOptions
            {
              public:
                HttpClientConnectionProxyOptions();
                HttpClientConnectionProxyOptions(const HttpClientConnectionProxyOptions &rhs) = default;
                HttpClientConnectionProxyOptions(HttpClientConnectionProxyOptions &&rhs) = default;

                HttpClientConnectionProxyOptions &operator=(const HttpClientConnectionProxyOptions &rhs) = default;
                HttpClientConnectionProxyOptions &operator=(HttpClientConnectionProxyOptions &&rhs) = default;

                ~HttpClientConnectionProxyOptions() = default;

                /**
                 * Intended for internal use only.  Initializes the C proxy configuration structure,
                 * aws_http_proxy_options, from an HttpClientConnectionProxyOptions instance.
                 *
                 * @param raw_options - output parameter containing low level proxy options to be passed to the C
                 * interface
                 *
                 */
                void InitializeRawProxyOptions(struct aws_http_proxy_options &raw_options) const;

                /**
                 * The name of the proxy server to connect through.
                 * Required.
                 */
                String HostName;

                /**
                 * The port of the proxy server to connect to.
                 * Required.
                 */
                uint32_t Port;

                /**
                 * Sets the TLS options for the connection to the proxy.
                 * Optional.
                 */
                Optional<Io::TlsConnectionOptions> TlsOptions;

                /**
                 * What kind of proxy connection to make
                 */
                AwsHttpProxyConnectionType ProxyConnectionType;

                /**
                 * Proxy strategy to use while negotiating the connection.  Use null for no additional
                 * steps.
                 */
                std::shared_ptr<HttpProxyStrategy> ProxyStrategy;

                /**
                 * @deprecated What kind of authentication approach to use when connecting to the proxy
                 * Replaced by proxy strategy
                 *
                 * Backwards compatibility achieved by invoking CreateBasicHttpProxyStrategy if
                 *   (1) ProxyStrategy is null
                 *   (2) AuthType is AwsHttpProxyAuthenticationType::Basic
                 */
                AwsHttpProxyAuthenticationType AuthType;

                /**
                 * @deprecated The username to use if connecting to the proxy via basic authentication
                 * Replaced by using the result of CreateBasicHttpProxyStrategy()
                 */
                String BasicAuthUsername;

                /**
                 * @deprecated The password to use if connecting to the proxy via basic authentication
                 * Replaced by using the result of CreateBasicHttpProxyStrategy()
                 */
                String BasicAuthPassword;
            };

            /**
             * Configuration structure holding all options relating to http connection establishment
             */
            class AWS_CRT_CPP_API HttpClientConnectionOptions
            {
              public:
                HttpClientConnectionOptions();
                HttpClientConnectionOptions(const HttpClientConnectionOptions &rhs) = default;
                HttpClientConnectionOptions(HttpClientConnectionOptions &&rhs) = default;

                ~HttpClientConnectionOptions() = default;

                HttpClientConnectionOptions &operator=(const HttpClientConnectionOptions &rhs) = default;
                HttpClientConnectionOptions &operator=(HttpClientConnectionOptions &&rhs) = default;

                /**
                 * The client bootstrap to use for setting up and tearing down connections.
                 * Note: If null, then the default ClientBootstrap is used
                 * (see Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap)
                 */
                Io::ClientBootstrap *Bootstrap;

                /**
                 *  The TCP read window allowed for Http 1.1 connections and Initial Windows for H2 connections.
                 */
                size_t InitialWindowSize;

                /**
                 * The callback invoked on connection establishment, whether success or failure.
                 * See `OnConnectionSetup` for more info.
                 * Required.
                 */
                OnConnectionSetup OnConnectionSetupCallback;

                /**
                 * The callback invoked on connection shutdown.
                 * See `OnConnectionShutdown` for more info.
                 * Required.
                 */
                OnConnectionShutdown OnConnectionShutdownCallback;

                /**
                 * The name of the http server to connect to.
                 * Required.
                 */
                String HostName;

                /**
                 * The port of the http server to connect to.
                 * Required.
                 */
                uint32_t Port;

                /**
                 * The socket options of the connection.
                 * Required.
                 */
                Io::SocketOptions SocketOptions;

                /**
                 * The TLS options for the http connection.
                 * Optional.
                 */
                Optional<Io::TlsConnectionOptions> TlsOptions;

                /**
                 * The proxy options for the http connection.
                 * Optional.
                 */
                Optional<HttpClientConnectionProxyOptions> ProxyOptions;

                /**
                 * If set to true, then the TCP read back pressure mechanism will be enabled. You should
                 * only use this if you're allowing http response body data to escape the callbacks. E.g. you're
                 * putting the data into a queue for another thread to process and need to make sure the memory
                 * usage is bounded. If this is enabled, you must call HttpStream::UpdateWindow() for every
                 * byte read from the OnIncomingBody callback.
                 */
                bool ManualWindowManagement;
            };

            enum class HttpVersion
            {
                Unknown = AWS_HTTP_VERSION_UNKNOWN,
                Http1_0 = AWS_HTTP_VERSION_1_0,
                Http1_1 = AWS_HTTP_VERSION_1_1,
                Http2 = AWS_HTTP_VERSION_2,
            };

            /**
             * Represents a connection from a Http Client to a Server.
             */
            class AWS_CRT_CPP_API HttpClientConnection : public std::enable_shared_from_this<HttpClientConnection>
            {
              public:
                virtual ~HttpClientConnection() = default;
                HttpClientConnection(const HttpClientConnection &) = delete;
                HttpClientConnection(HttpClientConnection &&) = delete;
                HttpClientConnection &operator=(const HttpClientConnection &) = delete;
                HttpClientConnection &operator=(HttpClientConnection &&) = delete;

                /**
                 * Make a new client initiated request on this connection.
                 *
                 * If you take a reference to the return value, the memory and resources for the connection
                 * and stream will not be cleaned up until you release it. You can however, release the reference
                 * as soon as you don't need it anymore. The internal reference count ensures the resources will
                 * not be freed until the stream is completed.
                 *
                 * Returns an instance of HttpStream upon success and nullptr on failure.
                 *
                 * You must call HttpClientStream::Activate() to begin outgoing processing of the stream.
                 */
                std::shared_ptr<HttpClientStream> NewClientStream(const HttpRequestOptions &requestOptions) noexcept;

                /**
                 * @return true unless the connection is closed or closing.
                 */
                bool IsOpen() const noexcept;

                /**
                 * Initiate a shutdown of the connection. Sometimes, connections are persistent and you want
                 * to close them before shutting down your application or whatever is consuming this interface.
                 *
                 * Assuming `OnConnectionShutdown` has not already been invoked, it will be invoked as a result of this
                 * call.
                 */
                void Close() noexcept;

                /**
                 * @return protocol version the connection used
                 */
                HttpVersion GetVersion() noexcept;

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept { return m_lastError; }

                /**
                 * Create a new Https Connection to hostName:port, using `socketOptions` for tcp options and
                 * `tlsConnOptions` for TLS/SSL options. If `tlsConnOptions` is null http (plain-text) will be used.
                 *
                 * returns true on success, and false on failure. If false is returned, `onConnectionSetup` will not
                 * be invoked. On success, `onConnectionSetup` will be called, either with a connection, or an
                 * errorCode.
                 */
                static bool CreateConnection(
                    const HttpClientConnectionOptions &connectionOptions,
                    Allocator *allocator) noexcept;

              protected:
                HttpClientConnection(aws_http_connection *m_connection, Allocator *allocator) noexcept;
                aws_http_connection *m_connection;

              private:
                Allocator *m_allocator;
                int m_lastError;

                static void s_onClientConnectionSetup(
                    struct aws_http_connection *connection,
                    int error_code,
                    void *user_data) noexcept;
                static void s_onClientConnectionShutdown(
                    struct aws_http_connection *connection,
                    int error_code,
                    void *user_data) noexcept;
            };

        } // namespace Http
    } // namespace Crt
} // namespace Aws
