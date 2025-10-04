/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/GetShardIteratorRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

GetShardIteratorRequest::GetShardIteratorRequest() : 
    m_streamNameHasBeenSet(false),
    m_shardIdHasBeenSet(false),
    m_shardIteratorType(ShardIteratorType::NOT_SET),
    m_shardIteratorTypeHasBeenSet(false),
    m_startingSequenceNumberHasBeenSet(false),
    m_timestampHasBeenSet(false),
    m_streamARNHasBeenSet(false)
{
}

Aws::String GetShardIteratorRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamNameHasBeenSet)
  {
   payload.WithString("StreamName", m_streamName);

  }

  if(m_shardIdHasBeenSet)
  {
   payload.WithString("ShardId", m_shardId);

  }

  if(m_shardIteratorTypeHasBeenSet)
  {
   payload.WithString("ShardIteratorType", ShardIteratorTypeMapper::GetNameForShardIteratorType(m_shardIteratorType));
  }

  if(m_startingSequenceNumberHasBeenSet)
  {
   payload.WithString("StartingSequenceNumber", m_startingSequenceNumber);

  }

  if(m_timestampHasBeenSet)
  {
   payload.WithDouble("Timestamp", m_timestamp.SecondsWithMSPrecision());
  }

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection GetShardIteratorRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.GetShardIterator"));
  return headers;

}



GetShardIteratorRequest::EndpointParameters GetShardIteratorRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("OperationType"), "data", Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    // Operation context parameters
    if (StreamARNHasBeenSet()) {
        parameters.emplace_back(Aws::String("StreamARN"), this->GetStreamARN(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}


