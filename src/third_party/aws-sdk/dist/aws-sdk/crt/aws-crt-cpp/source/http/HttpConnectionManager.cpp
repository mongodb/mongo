/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Api.h>
#include <aws/crt/http/HttpConnectionManager.h>
#include <aws/crt/http/HttpProxyStrategy.h>

#include <algorithm>
#include <aws/http/connection_manager.h>

namespace Aws
{
    namespace Crt
    {
        namespace Http
        {
            struct ConnectionManagerCallbackArgs
            {
                ConnectionManagerCallbackArgs() = default;
                OnClientConnectionAvailable m_onClientConnectionAvailable;
                std::shared_ptr<HttpClientConnectionManager> m_connectionManager;
            };

            void HttpClientConnectionManager::s_shutdownCompleted(void *userData) noexcept
            {
                HttpClientConnectionManager *connectionManager =
                    reinterpret_cast<HttpClientConnectionManager *>(userData);
                connectionManager->m_shutdownPromise.set_value();
            }

            HttpClientConnectionManagerOptions::HttpClientConnectionManagerOptions() noexcept
                : ConnectionOptions(), MaxConnections(1), EnableBlockingShutdown(false)
            {
            }

            std::shared_ptr<HttpClientConnectionManager> HttpClientConnectionManager::NewClientConnectionManager(
                const HttpClientConnectionManagerOptions &connectionManagerOptions,
                Allocator *allocator) noexcept
            {
                const Optional<Io::TlsConnectionOptions> &tlsOptions =
                    connectionManagerOptions.ConnectionOptions.TlsOptions;

                if (tlsOptions && !(*tlsOptions))
                {
                    AWS_LOGF_ERROR(
                        AWS_LS_HTTP_GENERAL,
                        "Cannot create HttpClientConnectionManager: ConnectionOptions contain invalid TLSOptions.");
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    return nullptr;
                }

                const Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> &proxyOptions =
                    connectionManagerOptions.ConnectionOptions.ProxyOptions;

                if (proxyOptions && proxyOptions->TlsOptions && !(*proxyOptions->TlsOptions))
                {
                    AWS_LOGF_ERROR(
                        AWS_LS_HTTP_GENERAL,
                        "Cannot create HttpClientConnectionManager: ProxyOptions has ConnectionOptions that contain "
                        "invalid TLSOptions.");
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    return nullptr;
                }

                auto *toSeat = static_cast<HttpClientConnectionManager *>(
                    aws_mem_acquire(allocator, sizeof(HttpClientConnectionManager)));
                if (toSeat)
                {
                    toSeat = new (toSeat) HttpClientConnectionManager(connectionManagerOptions, allocator);
                    return std::shared_ptr<HttpClientConnectionManager>(
                        toSeat, [allocator](HttpClientConnectionManager *manager) { Delete(manager, allocator); });
                }

                return nullptr;
            }

            HttpClientConnectionManager::HttpClientConnectionManager(
                const HttpClientConnectionManagerOptions &options,
                Allocator *allocator) noexcept
                : m_allocator(allocator), m_connectionManager(nullptr), m_options(options), m_releaseInvoked(false)
            {
                const auto &connectionOptions = m_options.ConnectionOptions;
                AWS_FATAL_ASSERT(connectionOptions.HostName.size() > 0);
                AWS_FATAL_ASSERT(connectionOptions.Port > 0);

                aws_http_connection_manager_options managerOptions;
                AWS_ZERO_STRUCT(managerOptions);

                if (connectionOptions.Bootstrap != nullptr)
                {
                    managerOptions.bootstrap = connectionOptions.Bootstrap->GetUnderlyingHandle();
                }
                else
                {
                    managerOptions.bootstrap =
                        ApiHandle::GetOrCreateStaticDefaultClientBootstrap()->GetUnderlyingHandle();
                }

                managerOptions.port = connectionOptions.Port;
                managerOptions.max_connections = m_options.MaxConnections;
                managerOptions.socket_options = &connectionOptions.SocketOptions.GetImpl();
                managerOptions.initial_window_size = connectionOptions.InitialWindowSize;

                if (options.EnableBlockingShutdown)
                {
                    managerOptions.shutdown_complete_callback = s_shutdownCompleted;
                    managerOptions.shutdown_complete_user_data = this;
                }
                else
                {
                    m_shutdownPromise.set_value();
                }

                aws_http_proxy_options proxyOptions;
                AWS_ZERO_STRUCT(proxyOptions);
                if (connectionOptions.ProxyOptions)
                {
                    /* This is verified by HttpClientConnectionManager::NewClientConnectionManager */
                    AWS_FATAL_ASSERT(
                        !connectionOptions.ProxyOptions->TlsOptions || *connectionOptions.ProxyOptions->TlsOptions);

                    const auto &proxyOpts = connectionOptions.ProxyOptions.value();
                    proxyOpts.InitializeRawProxyOptions(proxyOptions);

                    managerOptions.proxy_options = &proxyOptions;
                }

                if (connectionOptions.TlsOptions)
                {
                    /* This is verified by HttpClientConnectionManager::NewClientConnectionManager */
                    AWS_FATAL_ASSERT(*connectionOptions.TlsOptions);

                    managerOptions.tls_connection_options =
                        const_cast<aws_tls_connection_options *>(connectionOptions.TlsOptions->GetUnderlyingHandle());
                }
                managerOptions.host = aws_byte_cursor_from_c_str(connectionOptions.HostName.c_str());

                m_connectionManager = aws_http_connection_manager_new(allocator, &managerOptions);
            }

            HttpClientConnectionManager::~HttpClientConnectionManager()
            {
                if (!m_releaseInvoked)
                {
                    aws_http_connection_manager_release(m_connectionManager);
                    m_shutdownPromise.get_future().get();
                }
                m_connectionManager = nullptr;
            }

            bool HttpClientConnectionManager::AcquireConnection(
                const OnClientConnectionAvailable &onClientConnectionAvailable) noexcept
            {
                auto connectionManagerCallbackArgs = Aws::Crt::New<ConnectionManagerCallbackArgs>(m_allocator);
                if (!connectionManagerCallbackArgs)
                {
                    return false;
                }

                connectionManagerCallbackArgs->m_connectionManager = shared_from_this();
                connectionManagerCallbackArgs->m_onClientConnectionAvailable = onClientConnectionAvailable;

                aws_http_connection_manager_acquire_connection(
                    m_connectionManager, s_onConnectionSetup, connectionManagerCallbackArgs);
                return true;
            }

            std::future<void> HttpClientConnectionManager::InitiateShutdown() noexcept
            {
                m_releaseInvoked = true;
                aws_http_connection_manager_release(m_connectionManager);
                return m_shutdownPromise.get_future();
            }

            class ManagedConnection final : public HttpClientConnection
            {
              public:
                ManagedConnection(
                    aws_http_connection *connection,
                    std::shared_ptr<HttpClientConnectionManager> connectionManager)
                    : HttpClientConnection(connection, connectionManager->m_allocator),
                      m_connectionManager(std::move(connectionManager))
                {
                }

                ~ManagedConnection() override
                {
                    if (m_connection)
                    {
                        aws_http_connection_manager_release_connection(
                            m_connectionManager->m_connectionManager, m_connection);
                        m_connection = nullptr;
                    }
                }

              private:
                std::shared_ptr<HttpClientConnectionManager> m_connectionManager;
            };

            void HttpClientConnectionManager::s_onConnectionSetup(
                aws_http_connection *connection,
                int errorCode,
                void *userData) noexcept
            {
                auto callbackArgs = static_cast<ConnectionManagerCallbackArgs *>(userData);
                std::shared_ptr<HttpClientConnectionManager> manager = callbackArgs->m_connectionManager;
                auto callback = std::move(callbackArgs->m_onClientConnectionAvailable);

                Delete(callbackArgs, manager->m_allocator);

                if (errorCode)
                {
                    callback(nullptr, errorCode);
                    return;
                }

                auto allocator = manager->m_allocator;
                auto connectionRawObj = Aws::Crt::New<ManagedConnection>(manager->m_allocator, connection, manager);

                if (!connectionRawObj)
                {
                    aws_http_connection_manager_release_connection(manager->m_connectionManager, connection);
                    callback(nullptr, AWS_ERROR_OOM);
                    return;
                }
                auto connectionObj = std::shared_ptr<ManagedConnection>(
                    connectionRawObj,
                    [allocator](ManagedConnection *managedConnection) { Delete(managedConnection, allocator); });

                callback(connectionObj, AWS_OP_SUCCESS);
            }

        } // namespace Http
    } // namespace Crt
} // namespace Aws
