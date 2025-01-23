/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/UpdateStreamModeRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

UpdateStreamModeRequest::UpdateStreamModeRequest() : 
    m_streamARNHasBeenSet(false),
    m_streamModeDetailsHasBeenSet(false)
{
}

Aws::String UpdateStreamModeRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  if(m_streamModeDetailsHasBeenSet)
  {
   payload.WithObject("StreamModeDetails", m_streamModeDetails.Jsonize());

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection UpdateStreamModeRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.UpdateStreamMode"));
  return headers;

}



UpdateStreamModeRequest::EndpointParameters UpdateStreamModeRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("OperationType"), "control", Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    // Operation context parameters
    if (StreamARNHasBeenSet()) {
        parameters.emplace_back(Aws::String("StreamARN"), this->GetStreamARN(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}


