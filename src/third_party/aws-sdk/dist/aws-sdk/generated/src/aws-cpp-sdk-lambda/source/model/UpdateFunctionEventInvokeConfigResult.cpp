/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/UpdateFunctionEventInvokeConfigResult.h>
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

UpdateFunctionEventInvokeConfigResult::UpdateFunctionEventInvokeConfigResult() : 
    m_maximumRetryAttempts(0),
    m_maximumEventAgeInSeconds(0)
{
}

UpdateFunctionEventInvokeConfigResult::UpdateFunctionEventInvokeConfigResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : UpdateFunctionEventInvokeConfigResult()
{
  *this = result;
}

UpdateFunctionEventInvokeConfigResult& UpdateFunctionEventInvokeConfigResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("LastModified"))
  {
    m_lastModified = jsonValue.GetDouble("LastModified");

  }

  if(jsonValue.ValueExists("FunctionArn"))
  {
    m_functionArn = jsonValue.GetString("FunctionArn");

  }

  if(jsonValue.ValueExists("MaximumRetryAttempts"))
  {
    m_maximumRetryAttempts = jsonValue.GetInteger("MaximumRetryAttempts");

  }

  if(jsonValue.ValueExists("MaximumEventAgeInSeconds"))
  {
    m_maximumEventAgeInSeconds = jsonValue.GetInteger("MaximumEventAgeInSeconds");

  }

  if(jsonValue.ValueExists("DestinationConfig"))
  {
    m_destinationConfig = jsonValue.GetObject("DestinationConfig");

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
