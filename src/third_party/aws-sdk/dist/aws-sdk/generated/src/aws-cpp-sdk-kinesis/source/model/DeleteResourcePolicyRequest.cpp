/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/DeleteResourcePolicyRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

DeleteResourcePolicyRequest::DeleteResourcePolicyRequest() : 
    m_resourceARNHasBeenSet(false)
{
}

Aws::String DeleteResourcePolicyRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_resourceARNHasBeenSet)
  {
   payload.WithString("ResourceARN", m_resourceARN);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection DeleteResourcePolicyRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.DeleteResourcePolicy"));
  return headers;

}



DeleteResourcePolicyRequest::EndpointParameters DeleteResourcePolicyRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("OperationType"), "control", Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    // Operation context parameters
    if (ResourceARNHasBeenSet()) {
        parameters.emplace_back(Aws::String("ResourceARN"), this->GetResourceARN(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}


