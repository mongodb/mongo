/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/GetFunctionRecursionConfigResult.h>
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

GetFunctionRecursionConfigResult::GetFunctionRecursionConfigResult() : 
    m_recursiveLoop(RecursiveLoop::NOT_SET)
{
}

GetFunctionRecursionConfigResult::GetFunctionRecursionConfigResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : GetFunctionRecursionConfigResult()
{
  *this = result;
}

GetFunctionRecursionConfigResult& GetFunctionRecursionConfigResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("RecursiveLoop"))
  {
    m_recursiveLoop = RecursiveLoopMapper::GetRecursiveLoopForName(jsonValue.GetString("RecursiveLoop"));

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
