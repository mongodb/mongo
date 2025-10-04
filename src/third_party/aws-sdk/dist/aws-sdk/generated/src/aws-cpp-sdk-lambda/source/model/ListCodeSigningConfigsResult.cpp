﻿/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ListCodeSigningConfigsResult.h>
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

ListCodeSigningConfigsResult::ListCodeSigningConfigsResult()
{
}

ListCodeSigningConfigsResult::ListCodeSigningConfigsResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

ListCodeSigningConfigsResult& ListCodeSigningConfigsResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("NextMarker"))
  {
    m_nextMarker = jsonValue.GetString("NextMarker");

  }

  if(jsonValue.ValueExists("CodeSigningConfigs"))
  {
    Aws::Utils::Array<JsonView> codeSigningConfigsJsonList = jsonValue.GetArray("CodeSigningConfigs");
    for(unsigned codeSigningConfigsIndex = 0; codeSigningConfigsIndex < codeSigningConfigsJsonList.GetLength(); ++codeSigningConfigsIndex)
    {
      m_codeSigningConfigs.push_back(codeSigningConfigsJsonList[codeSigningConfigsIndex].AsObject());
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
