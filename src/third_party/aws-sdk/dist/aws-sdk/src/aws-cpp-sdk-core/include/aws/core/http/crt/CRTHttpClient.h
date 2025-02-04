/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/http/HttpClient.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/client/ClientConfiguration.h>

#include <aws/crt/io/TlsOptions.h>
#include <aws/crt/http/HttpConnection.h>

namespace Aws
{
    namespace Crt
    {
        namespace Http
        {
            class HttpClientConnectionManager;
            class HttpClientConnectionOptions;
        }

        namespace Io
        {
            class ClientBootstrap;
        }
    }

    namespace Client
    {
        struct ClientConfiguration;
    } // namespace Client

    namespace Http
    {
        /**
         *  Common Runtime implementation of AWS SDK for C++ HttpClient interface.
         */
        class AWS_CORE_API CRTHttpClient : public HttpClient {
        public:
            using Base = HttpClient;

            /**
             * Initializes the client with relevant parameters from clientConfig.
             */
            CRTHttpClient(const Aws::Client::ClientConfiguration& clientConfig, Crt::Io::ClientBootstrap& bootstrap);
            ~CRTHttpClient() override;

            std::shared_ptr<HttpResponse> MakeRequest(const std::shared_ptr<HttpRequest>& request,
                Aws::Utils::RateLimits::RateLimiterInterface* readLimiter,
                Aws::Utils::RateLimits::RateLimiterInterface* writeLimiter) const override;

        private:
            // Yeah, I know, but someone made MakeRequest() const and didn't think about the fact that
            // making an HTTP request most certainly mutates state. It was me. I'm the person that did that, and
            // now we're stuck with it. Thanks me.
            mutable std::unordered_map<Aws::String, const std::shared_ptr<Crt::Http::HttpClientConnectionManager>> m_connectionPools;
            mutable std::mutex m_connectionPoolLock;

            Crt::Optional<Crt::Io::TlsContext> m_context;
            Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> m_proxyOptions;

            Crt::Io::ClientBootstrap& m_bootstrap;
            Client::ClientConfiguration m_configuration;

            std::shared_ptr<Crt::Http::HttpClientConnectionManager> GetWithCreateConnectionManagerForRequest(const std::shared_ptr<HttpRequest>& request, const Crt::Http::HttpClientConnectionOptions& connectionOptions) const;
            Crt::Http::HttpClientConnectionOptions CreateConnectionOptionsForRequest(const std::shared_ptr<HttpRequest>& request) const;
            void CheckAndInitializeProxySettings(const Aws::Client::ClientConfiguration& clientConfig);

            static Aws::String ResolveConnectionPoolKey(const URI& uri);
        };
    }
}
