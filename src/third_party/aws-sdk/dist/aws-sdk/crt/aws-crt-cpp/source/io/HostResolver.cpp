/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/io/HostResolver.h>

#include <aws/crt/io/EventLoopGroup.h>

#include <aws/common/string.h>
#include <aws/crt/Api.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            HostResolver::~HostResolver() {}

            DefaultHostResolver::DefaultHostResolver(
                EventLoopGroup &elGroup,
                size_t maxHosts,
                size_t maxTTL,
                Allocator *allocator) noexcept
                : m_resolver(nullptr), m_allocator(allocator), m_initialized(false)
            {
                AWS_ZERO_STRUCT(m_config);

                struct aws_host_resolver_default_options resolver_options;
                AWS_ZERO_STRUCT(resolver_options);
                resolver_options.max_entries = maxHosts;
                resolver_options.el_group = elGroup.GetUnderlyingHandle();

                m_resolver = aws_host_resolver_new_default(allocator, &resolver_options);
                if (m_resolver != nullptr)
                {
                    m_initialized = true;
                }

                m_config.impl = aws_default_dns_resolve;
                m_config.impl_data = nullptr;
                m_config.max_ttl = maxTTL;
            }

            DefaultHostResolver::DefaultHostResolver(size_t maxHosts, size_t maxTTL, Allocator *allocator) noexcept
                : DefaultHostResolver(
                      *Crt::ApiHandle::GetOrCreateStaticDefaultEventLoopGroup(),
                      maxHosts,
                      maxTTL,
                      allocator)
            {
            }

            DefaultHostResolver::~DefaultHostResolver()
            {
                aws_host_resolver_release(m_resolver);
                m_initialized = false;
            }

            /**
             * @private
             */
            struct DefaultHostResolveArgs
            {
                Allocator *allocator;
                HostResolver *resolver;
                OnHostResolved onResolved;
                aws_string *host;
            };

            void DefaultHostResolver::s_onHostResolved(
                struct aws_host_resolver *,
                const struct aws_string *hostName,
                int errCode,
                const struct aws_array_list *hostAddresses,
                void *userData)
            {
                DefaultHostResolveArgs *args = static_cast<DefaultHostResolveArgs *>(userData);

                size_t len = hostAddresses ? aws_array_list_length(hostAddresses) : 0;
                Vector<HostAddress> addresses;
                addresses.reserve(len);

                for (size_t i = 0; i < len; ++i)
                {
                    HostAddress *address_ptr = NULL;
                    aws_array_list_get_at_ptr(hostAddresses, reinterpret_cast<void **>(&address_ptr), i);
                    addresses.push_back(*address_ptr);
                }

                String host(aws_string_c_str(hostName), hostName->len);
                args->onResolved(*args->resolver, addresses, errCode);
                aws_string_destroy(args->host);
                Delete(args, args->allocator);
            }

            bool DefaultHostResolver::ResolveHost(const String &host, const OnHostResolved &onResolved) noexcept
            {
                DefaultHostResolveArgs *args = New<DefaultHostResolveArgs>(m_allocator);
                if (!args)
                {
                    return false;
                }

                args->host = aws_string_new_from_array(
                    m_allocator, reinterpret_cast<const uint8_t *>(host.data()), host.length());
                args->onResolved = onResolved;
                args->resolver = this;
                args->allocator = m_allocator;

                if (!args->host ||
                    aws_host_resolver_resolve_host(m_resolver, args->host, s_onHostResolved, &m_config, args))
                {
                    Delete(args, m_allocator);
                    return false;
                }

                return true;
            }
        } // namespace Io
    } // namespace Crt
} // namespace Aws
