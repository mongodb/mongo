#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/http/HttpConnection.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>

struct aws_http_connection_manager;

namespace Aws
{
    namespace Crt
    {
        namespace Http
        {
            /**
             * Invoked when a connection from the pool is available. If a connection was successfully obtained
             * the connection shared_ptr can be seated into your own copy of connection. If it failed, errorCode
             * will be non-zero.
             */
            using OnClientConnectionAvailable =
                std::function<void(std::shared_ptr<HttpClientConnection>, int errorCode)>;

            /**
             * Configuration struct containing all options related to connection manager behavior
             */
            class AWS_CRT_CPP_API HttpClientConnectionManagerOptions
            {
              public:
                HttpClientConnectionManagerOptions() noexcept;
                HttpClientConnectionManagerOptions(const HttpClientConnectionManagerOptions &rhs) = default;
                HttpClientConnectionManagerOptions(HttpClientConnectionManagerOptions &&rhs) = default;

                HttpClientConnectionManagerOptions &operator=(const HttpClientConnectionManagerOptions &rhs) = default;
                HttpClientConnectionManagerOptions &operator=(HttpClientConnectionManagerOptions &&rhs) = default;

                /**
                 * The http connection options to use for each connection created by the manager
                 */
                HttpClientConnectionOptions ConnectionOptions;

                /**
                 * The maximum number of connections the manager is allowed to create/manage
                 */
                size_t MaxConnections;

                /** If set, initiate shutdown will return a future that will allow a user to block until the
                 * connection manager has completely released all resources. This isn't necessary during the normal
                 * flow of an application, but it is useful for scenarios, such as tests, that need deterministic
                 * shutdown ordering. Be aware, if you use this anywhere other than the main thread, you will most
                 * likely cause a deadlock. If this is set, you MUST call InitiateShutdown() before releasing your last
                 * reference to the connection manager.
                 */
                bool EnableBlockingShutdown;
            };

            /**
             * Manages a pool of connections to a specific endpoint using the same socket and tls options.
             */
            class AWS_CRT_CPP_API HttpClientConnectionManager final
                : public std::enable_shared_from_this<HttpClientConnectionManager>
            {
              public:
                ~HttpClientConnectionManager();

                /**
                 * Acquires a connection from the pool. onClientConnectionAvailable will be invoked upon an available
                 * connection. Returns true if the connection request was successfully queued, returns false if it
                 * failed. On failure, onClientConnectionAvailable will not be invoked. After receiving a connection, it
                 * will automatically be cleaned up when your last reference to the shared_ptr is released.
                 *
                 * @param onClientConnectionAvailable callback to invoke when a connection becomes available or the
                 * acquisition attempt terminates
                 * @return true if the acquisition was successfully kicked off, false otherwise (no callback)
                 */
                bool AcquireConnection(const OnClientConnectionAvailable &onClientConnectionAvailable) noexcept;

                /**
                 * Starts shutdown of the connection manager. Returns a future to the connection manager's shutdown
                 * process. If EnableBlockingDestruct was enabled on the connection manager options, calling get() on
                 * the returned future will block until the last connection is released. If the option is not set, get()
                 * will immediately return.
                 * @return future which will complete when shutdown has completed
                 */
                std::future<void> InitiateShutdown() noexcept;

                /**
                 * Factory function for connection managers
                 *
                 * @param connectionManagerOptions connection manager configuration data
                 * @param allocator allocator to use
                 * @return a new connection manager instance
                 */
                static std::shared_ptr<HttpClientConnectionManager> NewClientConnectionManager(
                    const HttpClientConnectionManagerOptions &connectionManagerOptions,
                    Allocator *allocator = ApiAllocator()) noexcept;

              private:
                HttpClientConnectionManager(
                    const HttpClientConnectionManagerOptions &options,
                    Allocator *allocator = ApiAllocator()) noexcept;

                Allocator *m_allocator;

                aws_http_connection_manager *m_connectionManager;

                HttpClientConnectionManagerOptions m_options;
                std::promise<void> m_shutdownPromise;
                std::atomic<bool> m_releaseInvoked;

                static void s_onConnectionSetup(
                    aws_http_connection *connection,
                    int errorCode,
                    void *userData) noexcept;

                static void s_shutdownCompleted(void *userData) noexcept;

                friend class ManagedConnection;
            };
        } // namespace Http
    } // namespace Crt
} // namespace Aws
