/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/GetEventSourceMappingRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

GetEventSourceMappingRequest::GetEventSourceMappingRequest() : 
    m_uUIDHasBeenSet(false)
{
}

Aws::String GetEventSourceMappingRequest::SerializePayload() const
{
  return {};
}




