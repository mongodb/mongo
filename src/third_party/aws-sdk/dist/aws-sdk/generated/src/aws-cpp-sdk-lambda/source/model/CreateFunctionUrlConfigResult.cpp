/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/CreateFunctionUrlConfigResult.h>
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

CreateFunctionUrlConfigResult::CreateFunctionUrlConfigResult() : 
    m_authType(FunctionUrlAuthType::NOT_SET),
    m_invokeMode(InvokeMode::NOT_SET)
{
}

CreateFunctionUrlConfigResult::CreateFunctionUrlConfigResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : CreateFunctionUrlConfigResult()
{
  *this = result;
}

CreateFunctionUrlConfigResult& CreateFunctionUrlConfigResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("FunctionUrl"))
  {
    m_functionUrl = jsonValue.GetString("FunctionUrl");

  }

  if(jsonValue.ValueExists("FunctionArn"))
  {
    m_functionArn = jsonValue.GetString("FunctionArn");

  }

  if(jsonValue.ValueExists("AuthType"))
  {
    m_authType = FunctionUrlAuthTypeMapper::GetFunctionUrlAuthTypeForName(jsonValue.GetString("AuthType"));

  }

  if(jsonValue.ValueExists("Cors"))
  {
    m_cors = jsonValue.GetObject("Cors");

  }

  if(jsonValue.ValueExists("CreationTime"))
  {
    m_creationTime = jsonValue.GetString("CreationTime");

  }

  if(jsonValue.ValueExists("InvokeMode"))
  {
    m_invokeMode = InvokeModeMapper::GetInvokeModeForName(jsonValue.GetString("InvokeMode"));

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
