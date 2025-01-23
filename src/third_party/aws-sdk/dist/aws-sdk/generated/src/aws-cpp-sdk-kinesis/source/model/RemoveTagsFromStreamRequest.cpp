/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/RemoveTagsFromStreamRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

RemoveTagsFromStreamRequest::RemoveTagsFromStreamRequest() : 
    m_streamNameHasBeenSet(false),
    m_tagKeysHasBeenSet(false),
    m_streamARNHasBeenSet(false)
{
}

Aws::String RemoveTagsFromStreamRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamNameHasBeenSet)
  {
   payload.WithString("StreamName", m_streamName);

  }

  if(m_tagKeysHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> tagKeysJsonList(m_tagKeys.size());
   for(unsigned tagKeysIndex = 0; tagKeysIndex < tagKeysJsonList.GetLength(); ++tagKeysIndex)
   {
     tagKeysJsonList[tagKeysIndex].AsString(m_tagKeys[tagKeysIndex]);
   }
   payload.WithArray("TagKeys", std::move(tagKeysJsonList));

  }

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection RemoveTagsFromStreamRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.RemoveTagsFromStream"));
  return headers;

}



RemoveTagsFromStreamRequest::EndpointParameters RemoveTagsFromStreamRequest::GetEndpointContextParams() const
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


