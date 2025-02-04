/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/PutFunctionEventInvokeConfigRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

PutFunctionEventInvokeConfigRequest::PutFunctionEventInvokeConfigRequest() : 
    m_functionNameHasBeenSet(false),
    m_qualifierHasBeenSet(false),
    m_maximumRetryAttempts(0),
    m_maximumRetryAttemptsHasBeenSet(false),
    m_maximumEventAgeInSeconds(0),
    m_maximumEventAgeInSecondsHasBeenSet(false),
    m_destinationConfigHasBeenSet(false)
{
}

Aws::String PutFunctionEventInvokeConfigRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_maximumRetryAttemptsHasBeenSet)
  {
   payload.WithInteger("MaximumRetryAttempts", m_maximumRetryAttempts);

  }

  if(m_maximumEventAgeInSecondsHasBeenSet)
  {
   payload.WithInteger("MaximumEventAgeInSeconds", m_maximumEventAgeInSeconds);

  }

  if(m_destinationConfigHasBeenSet)
  {
   payload.WithObject("DestinationConfig", m_destinationConfig.Jsonize());

  }

  return payload.View().WriteReadable();
}

void PutFunctionEventInvokeConfigRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_qualifierHasBeenSet)
    {
      ss << m_qualifier;
      uri.AddQueryStringParameter("Qualifier", ss.str());
      ss.str("");
    }

}



