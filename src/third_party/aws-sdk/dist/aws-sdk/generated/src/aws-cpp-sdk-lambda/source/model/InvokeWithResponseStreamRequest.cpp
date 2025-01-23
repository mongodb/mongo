/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/InvokeWithResponseStreamRequest.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/HashingUtils.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Stream;
using namespace Aws::Utils;
using namespace Aws::Http;
using namespace Aws;

InvokeWithResponseStreamRequest::InvokeWithResponseStreamRequest() : 
    m_functionNameHasBeenSet(false),
    m_invocationType(ResponseStreamingInvocationType::NOT_SET),
    m_invocationTypeHasBeenSet(false),
    m_logType(LogType::NOT_SET),
    m_logTypeHasBeenSet(false),
    m_clientContextHasBeenSet(false),
    m_qualifierHasBeenSet(false),
    m_handler(), m_decoder(Aws::Utils::Event::EventStreamDecoder(&m_handler))
{
    AmazonWebServiceRequest::SetHeadersReceivedEventHandler([this](const Http::HttpRequest*, Http::HttpResponse* response)
    {
        auto& initialResponseHandler = m_handler.GetInitialResponseCallbackEx();
        if (initialResponseHandler) {
            initialResponseHandler(InvokeWithResponseStreamInitialResponse(response->GetHeaders()), Utils::Event::InitialResponseType::ON_RESPONSE);
        }
    });
}


void InvokeWithResponseStreamRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_qualifierHasBeenSet)
    {
      ss << m_qualifier;
      uri.AddQueryStringParameter("Qualifier", ss.str());
      ss.str("");
    }

}

Aws::Http::HeaderValueCollection InvokeWithResponseStreamRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
  if(m_invocationTypeHasBeenSet && m_invocationType != ResponseStreamingInvocationType::NOT_SET)
  {
    headers.emplace("x-amz-invocation-type", ResponseStreamingInvocationTypeMapper::GetNameForResponseStreamingInvocationType(m_invocationType));
  }

  if(m_logTypeHasBeenSet && m_logType != LogType::NOT_SET)
  {
    headers.emplace("x-amz-log-type", LogTypeMapper::GetNameForLogType(m_logType));
  }

  if(m_clientContextHasBeenSet)
  {
    ss << m_clientContext;
    headers.emplace("x-amz-client-context",  ss.str());
    ss.str("");
  }

  return headers;

}
