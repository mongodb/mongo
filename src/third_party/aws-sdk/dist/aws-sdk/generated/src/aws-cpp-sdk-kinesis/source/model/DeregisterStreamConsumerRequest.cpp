/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/DeregisterStreamConsumerRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

DeregisterStreamConsumerRequest::DeregisterStreamConsumerRequest() : 
    m_streamARNHasBeenSet(false),
    m_consumerNameHasBeenSet(false),
    m_consumerARNHasBeenSet(false)
{
}

Aws::String DeregisterStreamConsumerRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  if(m_consumerNameHasBeenSet)
  {
   payload.WithString("ConsumerName", m_consumerName);

  }

  if(m_consumerARNHasBeenSet)
  {
   payload.WithString("ConsumerARN", m_consumerARN);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection DeregisterStreamConsumerRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.DeregisterStreamConsumer"));
  return headers;

}



DeregisterStreamConsumerRequest::EndpointParameters DeregisterStreamConsumerRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("OperationType"), "control", Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    // Operation context parameters
    if (StreamARNHasBeenSet()) {
        parameters.emplace_back(Aws::String("StreamARN"), this->GetStreamARN(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    if (ConsumerARNHasBeenSet()) {
        parameters.emplace_back(Aws::String("ConsumerARN"), this->GetConsumerARN(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}


