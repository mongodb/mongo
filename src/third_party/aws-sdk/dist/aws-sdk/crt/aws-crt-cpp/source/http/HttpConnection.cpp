/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Api.h>
#include <aws/crt/http/HttpConnection.h>
#include <aws/crt/http/HttpProxyStrategy.h>
#include <aws/crt/http/HttpRequestResponse.h>
#include <aws/crt/io/Bootstrap.h>

namespace Aws
{
    namespace Crt
    {
        namespace Http
        {
            /* This exists to handle aws_http_connection's shutdown callback, which might fire after
             * HttpClientConnection has been destroyed. */
            struct ConnectionCallbackData
            {
                explicit ConnectionCallbackData(Allocator *allocator) : allocator(allocator) {}
                std::weak_ptr<HttpClientConnection> connection;
                Allocator *allocator;
                OnConnectionSetup onConnectionSetup;
                OnConnectionShutdown onConnectionShutdown;
            };

            class UnmanagedConnection final : public HttpClientConnection
            {
              public:
                UnmanagedConnection(aws_http_connection *connection, Aws::Crt::Allocator *allocator)
                    : HttpClientConnection(connection, allocator)
                {
                }

                ~UnmanagedConnection() override
                {
                    if (m_connection)
                    {
                        aws_http_connection_release(m_connection);
                        m_connection = nullptr;
                    }
                }
            };

            void HttpClientConnection::s_onClientConnectionSetup(
                struct aws_http_connection *connection,
                int errorCode,
                void *user_data) noexcept
            {
                /**
                 * Allocate an HttpClientConnection and seat it to `ConnectionCallbackData`'s shared_ptr.
                 */
                auto *callbackData = static_cast<ConnectionCallbackData *>(user_data);
                if (!errorCode)
                {
                    auto connectionObj = std::allocate_shared<UnmanagedConnection>(
                        Aws::Crt::StlAllocator<UnmanagedConnection>(), connection, callbackData->allocator);

                    if (connectionObj)
                    {
                        callbackData->connection = connectionObj;
                        callbackData->onConnectionSetup(std::move(connectionObj), errorCode);
                        return;
                    }

                    aws_http_connection_release(connection);
                    errorCode = aws_last_error();
                }

                callbackData->onConnectionSetup(nullptr, errorCode);
                Delete(callbackData, callbackData->allocator);
            }

            void HttpClientConnection::s_onClientConnectionShutdown(
                struct aws_http_connection *connection,
                int errorCode,
                void *user_data) noexcept
            {
                (void)connection;
                auto *callbackData = static_cast<ConnectionCallbackData *>(user_data);

                /* Don't invoke callback if the connection object has expired. */
                if (auto connectionPtr = callbackData->connection.lock())
                {
                    callbackData->onConnectionShutdown(*connectionPtr, errorCode);
                }

                Delete(callbackData, callbackData->allocator);
            }

            bool HttpClientConnection::CreateConnection(
                const HttpClientConnectionOptions &connectionOptions,
                Allocator *allocator) noexcept
            {
                AWS_FATAL_ASSERT(connectionOptions.OnConnectionSetupCallback);
                AWS_FATAL_ASSERT(connectionOptions.OnConnectionShutdownCallback);

                if (connectionOptions.TlsOptions && !(*connectionOptions.TlsOptions))
                {
                    AWS_LOGF_ERROR(
                        AWS_LS_HTTP_GENERAL,
                        "Cannot create HttpClientConnection: connectionOptions contains invalid TlsOptions.");
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    return false;
                }

                if (connectionOptions.ProxyOptions)
                {
                    const auto &proxyOpts = connectionOptions.ProxyOptions.value();

                    if (proxyOpts.TlsOptions && !(*proxyOpts.TlsOptions))
                    {
                        AWS_LOGF_ERROR(
                            AWS_LS_HTTP_GENERAL,
                            "Cannot create HttpClientConnection: connectionOptions has ProxyOptions that contain "
                            "invalid TlsOptions.");
                        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                        return false;
                    }
                }

                auto *callbackData = New<ConnectionCallbackData>(allocator, allocator);

                if (!callbackData)
                {
                    return false;
                }
                callbackData->onConnectionShutdown = connectionOptions.OnConnectionShutdownCallback;
                callbackData->onConnectionSetup = connectionOptions.OnConnectionSetupCallback;

                aws_http_client_connection_options options;
                AWS_ZERO_STRUCT(options);
                options.self_size = sizeof(aws_http_client_connection_options);

                if (options.bootstrap != nullptr)
                {
                    options.bootstrap = connectionOptions.Bootstrap->GetUnderlyingHandle();
                }
                else
                {
                    options.bootstrap = ApiHandle::GetOrCreateStaticDefaultClientBootstrap()->GetUnderlyingHandle();
                }

                if (connectionOptions.TlsOptions)
                {
                    /* This is verified earlier in this function. */
                    AWS_FATAL_ASSERT(*connectionOptions.TlsOptions);

                    options.tls_options =
                        const_cast<aws_tls_connection_options *>(connectionOptions.TlsOptions->GetUnderlyingHandle());
                }
                options.allocator = allocator;
                options.user_data = callbackData;
                options.host_name = aws_byte_cursor_from_c_str(connectionOptions.HostName.c_str());
                options.port = connectionOptions.Port;
                options.initial_window_size = connectionOptions.InitialWindowSize;
                options.socket_options = &connectionOptions.SocketOptions.GetImpl();
                options.on_setup = HttpClientConnection::s_onClientConnectionSetup;
                options.on_shutdown = HttpClientConnection::s_onClientConnectionShutdown;
                options.manual_window_management = connectionOptions.ManualWindowManagement;

                aws_http_proxy_options proxyOptions;
                AWS_ZERO_STRUCT(proxyOptions);
                if (connectionOptions.ProxyOptions)
                {
                    const auto &proxyOpts = connectionOptions.ProxyOptions.value();

                    /* This is verified earlier in this function. */
                    AWS_FATAL_ASSERT(!proxyOpts.TlsOptions || *proxyOpts.TlsOptions);

                    proxyOpts.InitializeRawProxyOptions(proxyOptions);

                    options.proxy_options = &proxyOptions;
                }

                if (aws_http_client_connect(&options))
                {
                    Delete(callbackData, allocator);
                    return false;
                }

                return true;
            }

            HttpClientConnection::HttpClientConnection(aws_http_connection *connection, Allocator *allocator) noexcept
                : m_connection(connection), m_allocator(allocator), m_lastError(AWS_ERROR_SUCCESS)
            {
            }

            std::shared_ptr<HttpClientStream> HttpClientConnection::NewClientStream(
                const HttpRequestOptions &requestOptions) noexcept
            {
                AWS_ASSERT(requestOptions.onIncomingHeaders);
                AWS_ASSERT(requestOptions.onStreamComplete);

                aws_http_make_request_options options;
                AWS_ZERO_STRUCT(options);
                options.self_size = sizeof(aws_http_make_request_options);
                options.request = requestOptions.request->GetUnderlyingMessage();
                options.on_response_body = HttpStream::s_onIncomingBody;
                options.on_response_headers = HttpStream::s_onIncomingHeaders;
                options.on_response_header_block_done = HttpStream::s_onIncomingHeaderBlockDone;
                options.on_complete = HttpStream::s_onStreamComplete;

                /* Do the same ref counting trick we did with HttpClientConnection. We need to maintain a reference
                 * internally (regardless of what the user does), until the Stream shuts down. */
                auto *toSeat = static_cast<HttpClientStream *>(aws_mem_acquire(m_allocator, sizeof(HttpClientStream)));

                if (toSeat)
                {
                    toSeat = new (toSeat) HttpClientStream(this->shared_from_this());

                    Allocator *captureAllocator = m_allocator;
                    std::shared_ptr<HttpClientStream> stream(
                        toSeat,
                        [captureAllocator](HttpStream *stream) { Delete(stream, captureAllocator); },
                        StlAllocator<HttpClientStream>(captureAllocator));

                    stream->m_onIncomingBody = requestOptions.onIncomingBody;
                    stream->m_onIncomingHeaders = requestOptions.onIncomingHeaders;
                    stream->m_onIncomingHeadersBlockDone = requestOptions.onIncomingHeadersBlockDone;
                    stream->m_onStreamComplete = requestOptions.onStreamComplete;
                    stream->m_callbackData.allocator = m_allocator;

                    // we purposefully do not set m_callbackData::stream because we don't want the reference count
                    // incremented until the request is kicked off via HttpClientStream::Activate(). Activate()
                    // increments the ref count.
                    options.user_data = &stream->m_callbackData;
                    stream->m_stream = aws_http_connection_make_request(m_connection, &options);

                    if (!stream->m_stream)
                    {
                        stream = nullptr;
                        m_lastError = aws_last_error();
                        return nullptr;
                    }

                    return stream;
                }

                m_lastError = aws_last_error();
                return nullptr;
            }

            bool HttpClientConnection::IsOpen() const noexcept
            {
                return aws_http_connection_is_open(m_connection);
            }

            void HttpClientConnection::Close() noexcept
            {
                aws_http_connection_close(m_connection);
            }

            HttpVersion HttpClientConnection::GetVersion() noexcept
            {
                return (HttpVersion)aws_http_connection_get_version(m_connection);
            }

            int HttpStream::s_onIncomingHeaders(
                struct aws_http_stream *,
                enum aws_http_header_block headerBlock,
                const struct aws_http_header *headerArray,
                size_t numHeaders,
                void *userData) noexcept
            {
                auto callbackData = static_cast<ClientStreamCallbackData *>(userData);
                callbackData->stream->m_onIncomingHeaders(*callbackData->stream, headerBlock, headerArray, numHeaders);

                return AWS_OP_SUCCESS;
            }

            int HttpStream::s_onIncomingHeaderBlockDone(
                struct aws_http_stream *,
                enum aws_http_header_block headerBlock,
                void *userData) noexcept
            {
                auto callbackData = static_cast<ClientStreamCallbackData *>(userData);

                if (callbackData->stream->m_onIncomingHeadersBlockDone)
                {
                    callbackData->stream->m_onIncomingHeadersBlockDone(*callbackData->stream, headerBlock);
                }

                return AWS_OP_SUCCESS;
            }

            int HttpStream::s_onIncomingBody(
                struct aws_http_stream *,
                const struct aws_byte_cursor *data,
                void *userData) noexcept
            {
                auto callbackData = static_cast<ClientStreamCallbackData *>(userData);

                if (callbackData->stream->m_onIncomingBody)
                {
                    callbackData->stream->m_onIncomingBody(*callbackData->stream, *data);
                }

                return AWS_OP_SUCCESS;
            }

            void HttpStream::s_onStreamComplete(struct aws_http_stream *, int errorCode, void *userData) noexcept
            {
                auto callbackData = static_cast<ClientStreamCallbackData *>(userData);
                callbackData->stream->m_onStreamComplete(*callbackData->stream, errorCode);
                callbackData->stream = nullptr;
            }

            HttpStream::HttpStream(const std::shared_ptr<HttpClientConnection> &connection) noexcept
                : m_stream(nullptr), m_connection(connection)
            {
            }

            HttpStream::~HttpStream()
            {
                if (m_stream)
                {
                    aws_http_stream_release(m_stream);
                }

                if (m_connection)
                {
                    m_connection = nullptr;
                }
            }

            HttpClientConnection &HttpStream::GetConnection() const noexcept
            {
                return *m_connection;
            }

            HttpClientStream::HttpClientStream(const std::shared_ptr<HttpClientConnection> &connection) noexcept
                : HttpStream(connection)
            {
            }

            HttpClientStream::~HttpClientStream() {}

            int HttpClientStream::GetResponseStatusCode() const noexcept
            {
                int status = 0;
                if (!aws_http_stream_get_incoming_response_status(m_stream, &status))
                {
                    return status;
                }

                return -1;
            }

            bool HttpClientStream::Activate() noexcept
            {
                m_callbackData.stream = shared_from_this();
                if (aws_http_stream_activate(m_stream))
                {
                    m_callbackData.stream = nullptr;
                    return false;
                }

                return true;
            }

            void HttpStream::UpdateWindow(std::size_t incrementSize) noexcept
            {
                aws_http_stream_update_window(m_stream, incrementSize);
            }

            HttpClientConnectionProxyOptions::HttpClientConnectionProxyOptions()
                : HostName(), Port(0), TlsOptions(), ProxyConnectionType(AwsHttpProxyConnectionType::Legacy),
                  ProxyStrategy(), AuthType(AwsHttpProxyAuthenticationType::None)
            {
            }

            void HttpClientConnectionProxyOptions::InitializeRawProxyOptions(
                struct aws_http_proxy_options &rawOptions) const
            {
                AWS_ZERO_STRUCT(rawOptions);
                rawOptions.connection_type = (enum aws_http_proxy_connection_type)ProxyConnectionType;
                rawOptions.host = aws_byte_cursor_from_c_str(HostName.c_str());
                rawOptions.port = Port;

                if (TlsOptions.has_value())
                {
                    rawOptions.tls_options = TlsOptions->GetUnderlyingHandle();
                }

                if (ProxyStrategy)
                {
                    rawOptions.proxy_strategy = ProxyStrategy->GetUnderlyingHandle();
                }

                if (AuthType == AwsHttpProxyAuthenticationType::Basic)
                {
                    rawOptions.auth_type = AWS_HPAT_BASIC;
                    rawOptions.auth_username = ByteCursorFromCString(BasicAuthUsername.c_str());
                    rawOptions.auth_password = ByteCursorFromCString(BasicAuthPassword.c_str());
                }
            }

            HttpClientConnectionOptions::HttpClientConnectionOptions()
                : Bootstrap(nullptr), InitialWindowSize(SIZE_MAX), OnConnectionSetupCallback(),
                  OnConnectionShutdownCallback(), HostName(), Port(0), SocketOptions(), TlsOptions(), ProxyOptions(),
                  ManualWindowManagement(false)
            {
            }
        } // namespace Http
    } // namespace Crt
} // namespace Aws
