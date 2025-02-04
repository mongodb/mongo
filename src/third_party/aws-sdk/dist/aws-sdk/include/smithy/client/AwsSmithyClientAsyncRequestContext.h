/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/Smithy_EXPORTS.h>
#include <smithy/interceptor/InterceptorContext.h>

#include <aws/core/endpoint/AWSEndpoint.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/AmazonWebServiceRequest.h>

#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/Outcome.h>


namespace smithy
{
    namespace client
    {
        class AwsSmithyClientAsyncRequestContext
        {
        public:
            using AwsCoreError = Aws::Client::AWSError<Aws::Client::CoreErrors>;
            using HttpResponseOutcome = Aws::Utils::Outcome<std::shared_ptr<Aws::Http::HttpResponse>, AwsCoreError>;
            using ResponseHandlerFunc = std::function<void(HttpResponseOutcome&&)>;

            struct RequestInfo
            {
                Aws::Utils::DateTime ttl;
                long attempt;
                long maxAttempts;

                Aws::String ToString() const
                {
                    Aws::StringStream ss;
                    if (ttl.WasParseSuccessful() && ttl != Aws::Utils::DateTime())
                    {
                        assert(attempt > 1);
                        ss << "ttl=" << ttl.ToGmtString(Aws::Utils::DateFormat::ISO_8601_BASIC) << "; ";
                    }
                    ss << "attempt=" << attempt;
                    if (maxAttempts > 0)
                    {
                        ss << "; max=" << maxAttempts;
                    }
                    return ss.str();
                }

                explicit operator Aws::String() const
                {
                    return ToString();
                }
            };

            Aws::String m_invocationId;
            Aws::Http::HttpMethod m_method;
            const Aws::AmazonWebServiceRequest* m_pRequest; // optional

            RequestInfo m_requestInfo;
            Aws::String m_requestName;
            std::shared_ptr<Aws::Http::HttpRequest> m_httpRequest;
            AuthSchemeOption m_authSchemeOption;
            Aws::Endpoint::AWSEndpoint m_endpoint;

            Aws::Crt::Optional<AwsCoreError> m_lastError;

            size_t m_retryCount;
            Aws::Vector<void*> m_monitoringContexts;

            ResponseHandlerFunc m_responseHandler;
            std::shared_ptr<Aws::Utils::Threading::Executor> m_pExecutor;
            std::shared_ptr<interceptor::InterceptorContext> m_interceptorContext;
        };
    } // namespace client
} // namespace smithy
