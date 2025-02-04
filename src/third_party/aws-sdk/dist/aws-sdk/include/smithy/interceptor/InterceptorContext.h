/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/client/CoreErrors.h>

namespace smithy
{
    namespace interceptor
    {
        class InterceptorContext
        {
        public:
            explicit InterceptorContext(const Aws::AmazonWebServiceRequest& m_modeled_request)
                : m_modeledRequest(m_modeled_request)
            {
            }

            virtual ~InterceptorContext() = default;
            InterceptorContext(const InterceptorContext& other) = delete;
            InterceptorContext(InterceptorContext&& other) noexcept = delete;
            InterceptorContext& operator=(const InterceptorContext& other) = delete;
            InterceptorContext& operator=(InterceptorContext&& other) noexcept = delete;

            const Aws::AmazonWebServiceRequest& GetModeledRequest() const
            {
                return m_modeledRequest;
            }

            std::shared_ptr<Aws::Http::HttpRequest> GetTransmitRequest() const
            {
                return m_transmitRequest;
            }

            void SetTransmitRequest(const std::shared_ptr<Aws::Http::HttpRequest>& transmitRequest)
            {
                m_transmitRequest = transmitRequest;
            }

            std::shared_ptr<Aws::Http::HttpResponse> GetTransmitResponse() const
            {
                return m_transmitResponse;
            }

            void SetTransmitResponse(const std::shared_ptr<Aws::Http::HttpResponse>& transmitResponse)
            {
                m_transmitResponse = transmitResponse;
            }

            Aws::String GetAttribute(const Aws::String& key) const
            {
                return m_attributes.at(key);
            }

            void SetAttribute(const Aws::String& key, const Aws::String& value)
            {
                m_attributes.insert({key, value});
            }

        private:
            Aws::Map<Aws::String, Aws::String> m_attributes{};
            const Aws::AmazonWebServiceRequest& m_modeledRequest;
            std::shared_ptr<Aws::Http::HttpRequest> m_transmitRequest{nullptr};
            std::shared_ptr<Aws::Http::HttpResponse> m_transmitResponse{nullptr};
        };
    }
}
