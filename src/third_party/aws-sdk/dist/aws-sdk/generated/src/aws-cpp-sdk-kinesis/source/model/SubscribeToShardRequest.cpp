/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/SubscribeToShardRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

SubscribeToShardRequest::SubscribeToShardRequest() : 
    m_consumerARNHasBeenSet(false),
    m_shardIdHasBeenSet(false),
    m_startingPositionHasBeenSet(false),
    m_handler(), m_decoder(Aws::Utils::Event::EventStreamDecoder(&m_handler))
{
}

Aws::String SubscribeToShardRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_consumerARNHasBeenSet)
  {
   payload.WithString("ConsumerARN", m_consumerARN);

  }

  if(m_shardIdHasBeenSet)
  {
   payload.WithString("ShardId", m_shardId);

  }

  if(m_startingPositionHasBeenSet)
  {
   payload.WithObject("StartingPosition", m_startingPosition.Jsonize());

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection SubscribeToShardRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.SubscribeToShard"));
  return headers;

}



SubscribeToShardRequest::EndpointParameters SubscribeToShardRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("OperationType"), "data", Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    // Operation context parameters
    if (ConsumerARNHasBeenSet()) {
        parameters.emplace_back(Aws::String("ConsumerARN"), this->GetConsumerARN(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}


