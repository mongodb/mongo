#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Types.h>

#include <aws/io/host_resolver.h>

#include <functional>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            class EventLoopGroup;
            class HostResolver;

            using HostAddress = aws_host_address;

            /**
             * Invoked upon resolution of an address. You do not own the memory pointed to in addresses, if you persist
             * the data, copy it first. If errorCode is AWS_ERROR_SUCCESS, the operation succeeded. Otherwise, the
             * operation failed.
             */
            using OnHostResolved =
                std::function<void(HostResolver &resolver, const Vector<HostAddress> &addresses, int errorCode)>;

            /**
             * Simple interface for DNS name lookup implementations
             */
            class AWS_CRT_CPP_API HostResolver
            {
              public:
                virtual ~HostResolver();
                virtual bool ResolveHost(const String &host, const OnHostResolved &onResolved) noexcept = 0;

                /// @private
                virtual aws_host_resolver *GetUnderlyingHandle() noexcept = 0;
                /// @private
                virtual aws_host_resolution_config *GetConfig() noexcept = 0;
            };

            /**
             * A wrapper around the CRT default host resolution system that uses getaddrinfo() farmed off
             * to separate threads in order to resolve names.
             */
            class AWS_CRT_CPP_API DefaultHostResolver final : public HostResolver
            {
              public:
                /**
                 * Resolves DNS addresses.
                 *
                 * @param elGroup: EventLoopGroup to use.
                 * @param maxHosts: the number of unique hosts to maintain in the cache.
                 * @param maxTTL: how long to keep an address in the cache before evicting it.
                 * @param allocator memory allocator to use.
                 */
                DefaultHostResolver(
                    EventLoopGroup &elGroup,
                    size_t maxHosts,
                    size_t maxTTL,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Resolves DNS addresses using the default EventLoopGroup.
                 *
                 * For more information on the default EventLoopGroup see
                 * Aws::Crt::ApiHandle::GetOrCreateStaticDefaultEventLoopGroup
                 *
                 * @param maxHosts: the number of unique hosts to maintain in the cache.
                 * @param maxTTL: how long to keep an address in the cache before evicting it.
                 * @param allocator memory allocator to use.
                 */
                DefaultHostResolver(size_t maxHosts, size_t maxTTL, Allocator *allocator = ApiAllocator()) noexcept;

                ~DefaultHostResolver();
                DefaultHostResolver(const DefaultHostResolver &) = delete;
                DefaultHostResolver &operator=(const DefaultHostResolver &) = delete;
                DefaultHostResolver(DefaultHostResolver &&) = delete;
                DefaultHostResolver &operator=(DefaultHostResolver &&) = delete;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept { return m_initialized; }

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept { return aws_last_error(); }

                /**
                 * Kicks off an asynchronous resolution of host. onResolved will be invoked upon completion of the
                 * resolution.
                 * @return False, the resolution was not attempted. True, onResolved will be
                 * called with the result.
                 */
                bool ResolveHost(const String &host, const OnHostResolved &onResolved) noexcept override;

                /// @private
                aws_host_resolver *GetUnderlyingHandle() noexcept override { return m_resolver; }
                /// @private
                aws_host_resolution_config *GetConfig() noexcept override { return &m_config; }

              private:
                aws_host_resolver *m_resolver;
                aws_host_resolution_config m_config;
                Allocator *m_allocator;
                bool m_initialized;

                static void s_onHostResolved(
                    struct aws_host_resolver *resolver,
                    const struct aws_string *host_name,
                    int err_code,
                    const struct aws_array_list *host_addresses,
                    void *user_data);
            };
        } // namespace Io
    } // namespace Crt
} // namespace Aws
