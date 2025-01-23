/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/EnableEnhancedMonitoringRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

EnableEnhancedMonitoringRequest::EnableEnhancedMonitoringRequest() : 
    m_streamNameHasBeenSet(false),
    m_shardLevelMetricsHasBeenSet(false),
    m_streamARNHasBeenSet(false)
{
}

Aws::String EnableEnhancedMonitoringRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamNameHasBeenSet)
  {
   payload.WithString("StreamName", m_streamName);

  }

  if(m_shardLevelMetricsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> shardLevelMetricsJsonList(m_shardLevelMetrics.size());
   for(unsigned shardLevelMetricsIndex = 0; shardLevelMetricsIndex < shardLevelMetricsJsonList.GetLength(); ++shardLevelMetricsIndex)
   {
     shardLevelMetricsJsonList[shardLevelMetricsIndex].AsString(MetricsNameMapper::GetNameForMetricsName(m_shardLevelMetrics[shardLevelMetricsIndex]));
   }
   payload.WithArray("ShardLevelMetrics", std::move(shardLevelMetricsJsonList));

  }

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection EnableEnhancedMonitoringRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.EnableEnhancedMonitoring"));
  return headers;

}



EnableEnhancedMonitoringRequest::EndpointParameters EnableEnhancedMonitoringRequest::GetEndpointContextParams() const
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


