/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Api.h>
#include <aws/crt/io/Bootstrap.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {

            /**
             * @private
             * Holds the bootstrap's shutdown promise.
             * Lives until the bootstrap's shutdown-complete callback fires.
             */
            class ClientBootstrapCallbackData
            {
              private:
                Allocator *m_allocator;

              public:
                ClientBootstrapCallbackData(Allocator *allocator) : m_allocator(allocator) {}
                /**
                 * Promise for bootstrap's shutdown.
                 */
                std::promise<void> ShutdownPromise;
                /**
                 * User callback of bootstrap's shutdown-complete.
                 */
                OnClientBootstrapShutdownComplete ShutdownCallback;

                /**
                 * Internal callback of bootstrap's shutdown-complete
                 */
                static void OnShutdownComplete(void *userData)
                {
                    auto callbackData = static_cast<ClientBootstrapCallbackData *>(userData);

                    callbackData->ShutdownPromise.set_value();
                    if (callbackData->ShutdownCallback)
                    {
                        callbackData->ShutdownCallback();
                    }

                    Crt::Delete(callbackData, callbackData->m_allocator);
                }
            };

            ClientBootstrap::ClientBootstrap(
                EventLoopGroup &elGroup,
                HostResolver &resolver,
                Allocator *allocator) noexcept
                : m_bootstrap(nullptr), m_lastError(AWS_ERROR_SUCCESS),
                  m_callbackData(Crt::New<ClientBootstrapCallbackData>(allocator, allocator)),
                  m_enableBlockingShutdown(false)
            {
                m_shutdownFuture = m_callbackData->ShutdownPromise.get_future();

                aws_client_bootstrap_options options;
                options.event_loop_group = elGroup.GetUnderlyingHandle();
                options.host_resolution_config = resolver.GetConfig();
                options.host_resolver = resolver.GetUnderlyingHandle();
                options.on_shutdown_complete = ClientBootstrapCallbackData::OnShutdownComplete;
                options.user_data = m_callbackData.get();
                m_bootstrap = aws_client_bootstrap_new(allocator, &options);
                if (!m_bootstrap)
                {
                    m_lastError = aws_last_error();
                }
            }

            ClientBootstrap::ClientBootstrap(Allocator *allocator) noexcept
                : ClientBootstrap(
                      *Crt::ApiHandle::GetOrCreateStaticDefaultEventLoopGroup(),
                      *Crt::ApiHandle::GetOrCreateStaticDefaultHostResolver(),
                      allocator)
            {
            }

            ClientBootstrap::~ClientBootstrap()
            {
                if (m_bootstrap)
                {
                    // Release m_callbackData, it destroys itself when shutdown completes.
                    m_callbackData.release();

                    aws_client_bootstrap_release(m_bootstrap);
                    if (m_enableBlockingShutdown)
                    {
                        // If your program is stuck here, stop using EnableBlockingShutdown()
                        m_shutdownFuture.wait();
                    }
                }
            }

            ClientBootstrap::operator bool() const noexcept
            {
                return m_lastError == AWS_ERROR_SUCCESS;
            }

            int ClientBootstrap::LastError() const noexcept
            {
                return m_lastError;
            }

            void ClientBootstrap::SetShutdownCompleteCallback(OnClientBootstrapShutdownComplete callback)
            {
                m_callbackData->ShutdownCallback = std::move(callback);
            }

            void ClientBootstrap::EnableBlockingShutdown() noexcept
            {
                m_enableBlockingShutdown = true;
            }

            aws_client_bootstrap *ClientBootstrap::GetUnderlyingHandle() const noexcept
            {
                if (*this)
                {
                    return m_bootstrap;
                }

                return nullptr;
            }
        } // namespace Io
    } // namespace Crt
} // namespace Aws
