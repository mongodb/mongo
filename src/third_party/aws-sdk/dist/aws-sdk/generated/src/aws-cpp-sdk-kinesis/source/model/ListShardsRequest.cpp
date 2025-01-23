/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/ListShardsRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

ListShardsRequest::ListShardsRequest() : 
    m_streamNameHasBeenSet(false),
    m_nextTokenHasBeenSet(false),
    m_exclusiveStartShardIdHasBeenSet(false),
    m_maxResults(0),
    m_maxResultsHasBeenSet(false),
    m_streamCreationTimestampHasBeenSet(false),
    m_shardFilterHasBeenSet(false),
    m_streamARNHasBeenSet(false)
{
}

Aws::String ListShardsRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamNameHasBeenSet)
  {
   payload.WithString("StreamName", m_streamName);

  }

  if(m_nextTokenHasBeenSet)
  {
   payload.WithString("NextToken", m_nextToken);

  }

  if(m_exclusiveStartShardIdHasBeenSet)
  {
   payload.WithString("ExclusiveStartShardId", m_exclusiveStartShardId);

  }

  if(m_maxResultsHasBeenSet)
  {
   payload.WithInteger("MaxResults", m_maxResults);

  }

  if(m_streamCreationTimestampHasBeenSet)
  {
   payload.WithDouble("StreamCreationTimestamp", m_streamCreationTimestamp.SecondsWithMSPrecision());
  }

  if(m_shardFilterHasBeenSet)
  {
   payload.WithObject("ShardFilter", m_shardFilter.Jsonize());

  }

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection ListShardsRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.ListShards"));
  return headers;

}



ListShardsRequest::EndpointParameters ListShardsRequest::GetEndpointContextParams() const
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


