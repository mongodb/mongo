#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Types.h>

#include <memory>

struct aws_http_proxy_strategy;

namespace Aws
{
    namespace Crt
    {
        namespace Http
        {
            enum class AwsHttpProxyConnectionType;

            /**
             * Configuration for a proxy strategy that performs basic authentication
             */
            struct AWS_CRT_CPP_API HttpProxyStrategyBasicAuthConfig
            {
                HttpProxyStrategyBasicAuthConfig();

                /**
                 * Basic auth can be applied either to forwarding or tunneling proxy connections, but we need
                 * to know the type ahead of time
                 */
                AwsHttpProxyConnectionType ConnectionType;

                /**
                 * Username to apply to the basic authentication process
                 */
                String Username;

                /**
                 * Password to apply to the basic authentication process
                 */
                String Password;
            };

            using KerberosGetTokenFunction = std::function<bool(String &)>;
            using NtlmGetTokenFunction = std::function<bool(const String &, String &)>;

            /**
             * Configuration for a proxy strategy that attempts to use kerberos and ntlm, based on authentication
             * failure feedback from the proxy's responses to CONNECT attempts.  The kerberos/ntlm callbacks are
             * currently synchronous but invoked potentially from within event loop threads.  This is not optimal
             * but transitioning to fully async hasn't been a need yet.
             *
             * The adapative strategy will skip an authentication method whose callbacks are not supplied, so you
             * can use this for purely kerberos or ntlm as well.
             */
            struct AWS_CRT_CPP_API HttpProxyStrategyAdaptiveConfig
            {
                HttpProxyStrategyAdaptiveConfig() : KerberosGetToken(), NtlmGetCredential(), NtlmGetToken() {}

                /**
                 * User-supplied callback for fetching kerberos tokens
                 */
                KerberosGetTokenFunction KerberosGetToken;

                /**
                 * User-supplied callback for fetching an ntlm credential
                 */
                KerberosGetTokenFunction NtlmGetCredential;

                /**
                 * User-supplied callback for fetching an ntlm token
                 */
                NtlmGetTokenFunction NtlmGetToken;
            };

            /**
             * Wrapper class for a C-level proxy strategy - an object that allows the user to transform or modify
             * the authentication logic when connecting to a proxy.
             */
            class AWS_CRT_CPP_API HttpProxyStrategy
            {
              public:
                HttpProxyStrategy(struct aws_http_proxy_strategy *strategy);
                virtual ~HttpProxyStrategy();

                /// @private
                struct aws_http_proxy_strategy *GetUnderlyingHandle() const noexcept { return m_strategy; }

                /**
                 * Creates a proxy strategy that performs basic authentication
                 * @param config basic authentication configuration options
                 * @param allocator allocator to use
                 * @return a new basic authentication proxy strategy
                 */
                static std::shared_ptr<HttpProxyStrategy> CreateBasicHttpProxyStrategy(
                    const HttpProxyStrategyBasicAuthConfig &config,
                    Allocator *allocator = ApiAllocator());

                /**
                 * Creates a proxy strategy that, depending on configuration, can attempt kerberos and/or ntlm
                 * authentication when connecting to the proxy
                 * @param config the adaptive strategy configuration options
                 * @param allocator allocator to use
                 * @return a new adaptive proxy strategy
                 */
                static std::shared_ptr<HttpProxyStrategy> CreateAdaptiveHttpProxyStrategy(
                    const HttpProxyStrategyAdaptiveConfig &config,
                    Allocator *allocator = ApiAllocator());

              protected:
                struct aws_http_proxy_strategy *m_strategy;
            };
        } // namespace Http
    } // namespace Crt
} // namespace Aws
