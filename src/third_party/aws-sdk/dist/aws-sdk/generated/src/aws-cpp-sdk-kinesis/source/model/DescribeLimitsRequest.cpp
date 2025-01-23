/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/DescribeLimitsRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

DescribeLimitsRequest::DescribeLimitsRequest()
{
}

Aws::String DescribeLimitsRequest::SerializePayload() const
{
  return "{}";
}

Aws::Http::HeaderValueCollection DescribeLimitsRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.DescribeLimits"));
  return headers;

}




