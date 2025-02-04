/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/DecreaseStreamRetentionPeriodRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

DecreaseStreamRetentionPeriodRequest::DecreaseStreamRetentionPeriodRequest() : 
    m_streamNameHasBeenSet(false),
    m_retentionPeriodHours(0),
    m_retentionPeriodHoursHasBeenSet(false),
    m_streamARNHasBeenSet(false)
{
}

Aws::String DecreaseStreamRetentionPeriodRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamNameHasBeenSet)
  {
   payload.WithString("StreamName", m_streamName);

  }

  if(m_retentionPeriodHoursHasBeenSet)
  {
   payload.WithInteger("RetentionPeriodHours", m_retentionPeriodHours);

  }

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection DecreaseStreamRetentionPeriodRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.DecreaseStreamRetentionPeriod"));
  return headers;

}



DecreaseStreamRetentionPeriodRequest::EndpointParameters DecreaseStreamRetentionPeriodRequest::GetEndpointContextParams() const
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


