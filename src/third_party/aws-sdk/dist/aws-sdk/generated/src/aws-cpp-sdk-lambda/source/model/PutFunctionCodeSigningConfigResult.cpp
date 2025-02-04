/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/PutFunctionCodeSigningConfigResult.h>
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

PutFunctionCodeSigningConfigResult::PutFunctionCodeSigningConfigResult()
{
}

PutFunctionCodeSigningConfigResult::PutFunctionCodeSigningConfigResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

PutFunctionCodeSigningConfigResult& PutFunctionCodeSigningConfigResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("CodeSigningConfigArn"))
  {
    m_codeSigningConfigArn = jsonValue.GetString("CodeSigningConfigArn");

  }

  if(jsonValue.ValueExists("FunctionName"))
  {
    m_functionName = jsonValue.GetString("FunctionName");

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
