#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>
#include <aws/crt/io/EventLoopGroup.h>
#include <aws/crt/io/HostResolver.h>

#include <aws/io/channel_bootstrap.h>
#include <aws/io/host_resolver.h>

#include <future>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            using OnClientBootstrapShutdownComplete = std::function<void()>;

            /**
             * A ClientBootstrap handles creation and setup of socket connections
             * to specific endpoints.
             *
             * Note that ClientBootstrap may not clean up all its behind-the-scenes
             * resources immediately upon destruction. If you need to know when
             * behind-the-scenes shutdown is complete, use SetShutdownCompleteCallback()
             * or EnableBlockingShutdown() (only safe on main thread).
             */
            class AWS_CRT_CPP_API ClientBootstrap final
            {
              public:
                /**
                 * @param elGroup: EventLoopGroup to use.
                 * @param resolver: DNS host resolver to use.
                 * @param allocator memory allocator to use
                 */
                ClientBootstrap(
                    EventLoopGroup &elGroup,
                    HostResolver &resolver,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Uses the default EventLoopGroup and HostResolver.
                 * See Aws::Crt::ApiHandle::GetOrCreateStaticDefaultEventLoopGroup
                 * and Aws::Crt::ApiHandle::GetOrCreateStaticDefaultHostResolver
                 */
                ClientBootstrap(Allocator *allocator = ApiAllocator()) noexcept;

                ~ClientBootstrap();
                ClientBootstrap(const ClientBootstrap &) = delete;
                ClientBootstrap &operator=(const ClientBootstrap &) = delete;
                ClientBootstrap(ClientBootstrap &&) = delete;
                ClientBootstrap &operator=(ClientBootstrap &&) = delete;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept;

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept;

                /**
                 * Set function to invoke when ClientBootstrap's behind-the-scenes
                 * resources finish shutting down. This function may be invoked
                 * on any thread. Shutdown begins when the ClientBootstrap's
                 * destructor runs.
                 */
                void SetShutdownCompleteCallback(OnClientBootstrapShutdownComplete callback);

                /**
                 * Force the ClientBootstrap's destructor to block until all
                 * behind-the-scenes resources finish shutting down.
                 *
                 * This isn't necessary during the normal flow of an application,
                 * but it is useful for scenarios, such as tests, that need deterministic
                 * shutdown ordering. Be aware, if you use this anywhere other
                 * than the main thread, YOU WILL MOST LIKELY CAUSE A DEADLOCK.
                 *
                 * Use SetShutdownCompleteCallback() for a thread-safe way to
                 * know when shutdown is complete.
                 */
                void EnableBlockingShutdown() noexcept;

                /// @private
                aws_client_bootstrap *GetUnderlyingHandle() const noexcept;

              private:
                aws_client_bootstrap *m_bootstrap;
                int m_lastError;
                std::unique_ptr<class ClientBootstrapCallbackData> m_callbackData;
                std::future<void> m_shutdownFuture;
                bool m_enableBlockingShutdown;
            };
        } // namespace Io
    } // namespace Crt
} // namespace Aws
