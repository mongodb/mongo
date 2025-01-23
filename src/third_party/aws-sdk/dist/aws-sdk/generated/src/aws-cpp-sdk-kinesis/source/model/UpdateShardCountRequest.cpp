/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/UpdateShardCountRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

UpdateShardCountRequest::UpdateShardCountRequest() : 
    m_streamNameHasBeenSet(false),
    m_targetShardCount(0),
    m_targetShardCountHasBeenSet(false),
    m_scalingType(ScalingType::NOT_SET),
    m_scalingTypeHasBeenSet(false),
    m_streamARNHasBeenSet(false)
{
}

Aws::String UpdateShardCountRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamNameHasBeenSet)
  {
   payload.WithString("StreamName", m_streamName);

  }

  if(m_targetShardCountHasBeenSet)
  {
   payload.WithInteger("TargetShardCount", m_targetShardCount);

  }

  if(m_scalingTypeHasBeenSet)
  {
   payload.WithString("ScalingType", ScalingTypeMapper::GetNameForScalingType(m_scalingType));
  }

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection UpdateShardCountRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.UpdateShardCount"));
  return headers;

}



UpdateShardCountRequest::EndpointParameters UpdateShardCountRequest::GetEndpointContextParams() const
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


