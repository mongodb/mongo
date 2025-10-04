/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ListProvisionedConcurrencyConfigsResult.h>
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

ListProvisionedConcurrencyConfigsResult::ListProvisionedConcurrencyConfigsResult()
{
}

ListProvisionedConcurrencyConfigsResult::ListProvisionedConcurrencyConfigsResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

ListProvisionedConcurrencyConfigsResult& ListProvisionedConcurrencyConfigsResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("ProvisionedConcurrencyConfigs"))
  {
    Aws::Utils::Array<JsonView> provisionedConcurrencyConfigsJsonList = jsonValue.GetArray("ProvisionedConcurrencyConfigs");
    for(unsigned provisionedConcurrencyConfigsIndex = 0; provisionedConcurrencyConfigsIndex < provisionedConcurrencyConfigsJsonList.GetLength(); ++provisionedConcurrencyConfigsIndex)
    {
      m_provisionedConcurrencyConfigs.push_back(provisionedConcurrencyConfigsJsonList[provisionedConcurrencyConfigsIndex].AsObject());
    }
  }

  if(jsonValue.ValueExists("NextMarker"))
  {
    m_nextMarker = jsonValue.GetString("NextMarker");

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
