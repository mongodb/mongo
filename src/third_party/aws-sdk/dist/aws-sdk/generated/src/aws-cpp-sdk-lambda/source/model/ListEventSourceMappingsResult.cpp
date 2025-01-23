/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ListEventSourceMappingsResult.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws;

ListEventSourceMappingsResult::ListEventSourceMappingsResult()
{
}

ListEventSourceMappingsResult::ListEventSourceMappingsResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

ListEventSourceMappingsResult& ListEventSourceMappingsResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("NextMarker"))
  {
    m_nextMarker = jsonValue.GetString("NextMarker");

  }

  if(jsonValue.ValueExists("EventSourceMappings"))
  {
    Aws::Utils::Array<JsonView> eventSourceMappingsJsonList = jsonValue.GetArray("EventSourceMappings");
    for(unsigned eventSourceMappingsIndex = 0; eventSourceMappingsIndex < eventSourceMappingsJsonList.GetLength(); ++eventSourceMappingsIndex)
    {
      m_eventSourceMappings.push_back(eventSourceMappingsJsonList[eventSourceMappingsIndex].AsObject());
    }
  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
