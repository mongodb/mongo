/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/auth/AuthSchemeOption.h>
#include <smithy/identity/identity/AwsIdentity.h>
#include <smithy/tracing/TelemetryProvider.h>
#include <smithy/interceptor/Interceptor.h>
#include <smithy/client/features/ChecksumInterceptor.h>

#include <aws/crt/Variant.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/FutureOutcome.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/Outcome.h>

namespace Aws
{
    namespace Utils
    {
        namespace RateLimits
        {
            class RateLimiterInterface;
        }
    }

    namespace Http
    {
        class HttpClient;
    }

    namespace Client
    {
        class AWSErrorMarshaller;
        class RetryStrategy;
    }

    namespace Utils
    {
        namespace Threading
        {
            class Executor;
        }
    }

    class AmazonWebServiceRequest;
}

namespace Aws
{
    namespace Endpoint
    {
        class AWSEndpoint;
    }
}

namespace smithy {
namespace client
{
    class AwsSmithyClientAsyncRequestContext;
    /* Non-template base client class that contains main Aws Client Request pipeline logic */
    class SMITHY_API AwsSmithyClientBase
    {
    public:
        using HttpRequest = Aws::Http::HttpRequest;
        using HttpResponse = Aws::Http::HttpResponse;
        using CoreErrors = Aws::Client::CoreErrors;
        using AWSError = Aws::Client::AWSError<CoreErrors>;
        using ClientError = AWSError;
        using SigningError = AWSError;
        using SigningOutcome = Aws::Utils::FutureOutcome<std::shared_ptr<Aws::Http::HttpRequest>, SigningError>;
        using EndpointUpdateCallback = std::function<void(Aws::Endpoint::AWSEndpoint&)>;
        using HttpResponseOutcome = Aws::Utils::Outcome<std::shared_ptr<Aws::Http::HttpResponse>, AWSError>;
        using ResponseHandlerFunc = std::function<void(HttpResponseOutcome&&)>;
        using SelectAuthSchemeOptionOutcome = Aws::Utils::Outcome<AuthSchemeOption, AWSError>;
        using ResolveEndpointOutcome = Aws::Utils::Outcome<Aws::Endpoint::AWSEndpoint, AWSError>;

        AwsSmithyClientBase(Aws::UniquePtr<Aws::Client::ClientConfiguration>&& clientConfig,
                            Aws::String serviceName,
                            std::shared_ptr<Aws::Http::HttpClient> httpClient,
                            std::shared_ptr<Aws::Client::AWSErrorMarshaller> errorMarshaller) :
          m_clientConfig(std::move(clientConfig)),
          m_serviceName(std::move(serviceName)),
          m_userAgent(),
          m_httpClient(std::move(httpClient)),
          m_errorMarshaller(std::move(errorMarshaller)),
          m_interceptors{Aws::MakeShared<ChecksumInterceptor>("AwsSmithyClientBase")}
        {
            if (!m_clientConfig->retryStrategy)
            {
                assert(m_clientConfig->configFactories.retryStrategyCreateFn);
                m_clientConfig->retryStrategy = m_clientConfig->configFactories.retryStrategyCreateFn();
            }
            if (!m_clientConfig->executor)
            {
                assert(m_clientConfig->configFactories.executorCreateFn);
                m_clientConfig->executor = m_clientConfig->configFactories.executorCreateFn();
            }
            if (!m_clientConfig->writeRateLimiter)
            {
                assert(m_clientConfig->configFactories.writeRateLimiterCreateFn);
                m_clientConfig->writeRateLimiter = m_clientConfig->configFactories.writeRateLimiterCreateFn();
            }
            if (!m_clientConfig->readRateLimiter)
            {
                assert(m_clientConfig->configFactories.readRateLimiterCreateFn);
                m_clientConfig->readRateLimiter = m_clientConfig->configFactories.readRateLimiterCreateFn();
            }
            if (!m_clientConfig->telemetryProvider)
            {
                assert(m_clientConfig->configFactories.telemetryProviderCreateFn);
                m_clientConfig->telemetryProvider = m_clientConfig->configFactories.telemetryProviderCreateFn();
            }

            m_userAgent = Aws::Client::ComputeUserAgentString(m_clientConfig.get());
        }

        AwsSmithyClientBase(const AwsSmithyClientBase&) = delete;
        AwsSmithyClientBase(AwsSmithyClientBase&&) = delete;
        AwsSmithyClientBase& operator=(const AwsSmithyClientBase&) = delete;
        AwsSmithyClientBase& operator=(AwsSmithyClientBase&&) = delete;

        virtual ~AwsSmithyClientBase() = default;

        void MakeRequestAsync(Aws::AmazonWebServiceRequest const * const request,
                              const char* requestName,
                              Aws::Http::HttpMethod method,
                              EndpointUpdateCallback&& endpointCallback,
                              ResponseHandlerFunc&& responseHandler,
                              std::shared_ptr<Aws::Utils::Threading::Executor> pExecutor) const;

        HttpResponseOutcome MakeRequestSync(Aws::AmazonWebServiceRequest const * const request,
                                            const char* requestName,
                                            Aws::Http::HttpMethod method,
                                            EndpointUpdateCallback&& endpointCallback) const;

    protected:
        /**
         * Transforms the AmazonWebServicesResult object into an HttpRequest.
         */
        std::shared_ptr<Aws::Http::HttpRequest> BuildHttpRequest(const std::shared_ptr<AwsSmithyClientAsyncRequestContext>& pRequestCtx, const Aws::Http::URI& uri, Aws::Http::HttpMethod method) const;


        virtual void AttemptOneRequestAsync(std::shared_ptr<AwsSmithyClientAsyncRequestContext> pRequestCtx) const;

        virtual void HandleAsyncReply(std::shared_ptr<AwsSmithyClientAsyncRequestContext> pRequestCtx,
                                      std::shared_ptr<Aws::Http::HttpResponse> httpResponse) const;

        inline virtual const char* GetServiceClientName() const { return m_serviceName.c_str(); }
        inline virtual const std::shared_ptr<Aws::Http::HttpClient>& GetHttpClient() { return m_httpClient; }
        virtual void DisableRequestProcessing();

        virtual ResolveEndpointOutcome ResolveEndpoint(const Aws::Endpoint::EndpointParameters& endpointParameters, EndpointUpdateCallback&& epCallback) const = 0;
        virtual SelectAuthSchemeOptionOutcome SelectAuthSchemeOption(const AwsSmithyClientAsyncRequestContext& ctx) const = 0;
        virtual SigningOutcome SignRequest(std::shared_ptr<HttpRequest> httpRequest, const AuthSchemeOption& targetAuthSchemeOption) const = 0;
        virtual bool AdjustClockSkew(HttpResponseOutcome& outcome, const AuthSchemeOption& authSchemeOption) const = 0;

    protected:
        Aws::UniquePtr<Aws::Client::ClientConfiguration> m_clientConfig;
        Aws::String m_serviceName;
        Aws::String m_userAgent;

        std::shared_ptr<Aws::Http::HttpClient> m_httpClient;
        std::shared_ptr<Aws::Client::AWSErrorMarshaller> m_errorMarshaller;
        Aws::Vector<std::shared_ptr<smithy::interceptor::Interceptor>> m_interceptors{};
    };
} // namespace client
} // namespace smithy
