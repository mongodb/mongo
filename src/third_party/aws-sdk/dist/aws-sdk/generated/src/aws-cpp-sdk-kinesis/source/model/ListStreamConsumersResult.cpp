/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/ListStreamConsumersResult.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws;

ListStreamConsumersResult::ListStreamConsumersResult()
{
}

ListStreamConsumersResult::ListStreamConsumersResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

ListStreamConsumersResult& ListStreamConsumersResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("Consumers"))
  {
    Aws::Utils::Array<JsonView> consumersJsonList = jsonValue.GetArray("Consumers");
    for(unsigned consumersIndex = 0; consumersIndex < consumersJsonList.GetLength(); ++consumersIndex)
    {
      m_consumers.push_back(consumersJsonList[consumersIndex].AsObject());
    }
  }

  if(jsonValue.ValueExists("NextToken"))
  {
    m_nextToken = jsonValue.GetString("NextToken");

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
