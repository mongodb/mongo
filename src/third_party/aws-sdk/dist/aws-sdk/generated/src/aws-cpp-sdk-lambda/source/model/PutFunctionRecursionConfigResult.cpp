/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/PutFunctionRecursionConfigResult.h>
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

PutFunctionRecursionConfigResult::PutFunctionRecursionConfigResult() : 
    m_recursiveLoop(RecursiveLoop::NOT_SET)
{
}

PutFunctionRecursionConfigResult::PutFunctionRecursionConfigResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : PutFunctionRecursionConfigResult()
{
  *this = result;
}

PutFunctionRecursionConfigResult& PutFunctionRecursionConfigResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
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
