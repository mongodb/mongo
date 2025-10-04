/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws;

AmazonWebServiceRequest::AmazonWebServiceRequest() :
    m_responseStreamFactory(Aws::Utils::Stream::DefaultResponseStreamFactoryMethod),
    m_onDataReceived(nullptr),
    m_onDataSent(nullptr),
    m_continueRequest(nullptr),
    m_onRequestSigned(nullptr),
    m_requestRetryHandler(nullptr)
{
}

AmazonWebServiceRequest::EndpointParameters AmazonWebServiceRequest::GetEndpointContextParams() const
{
    return AmazonWebServiceRequest::EndpointParameters();
}

const Aws::Http::HeaderValueCollection& AmazonWebServiceRequest::GetAdditionalCustomHeaders() const
{
    return m_additionalCustomHeaders;
}

void AmazonWebServiceRequest::SetAdditionalCustomHeaderValue(const Aws::String& headerName, const Aws::String& headerValue)
{
    m_additionalCustomHeaders[Utils::StringUtils::ToLower(headerName.c_str())] = Utils::StringUtils::Trim(headerValue.c_str());
}
