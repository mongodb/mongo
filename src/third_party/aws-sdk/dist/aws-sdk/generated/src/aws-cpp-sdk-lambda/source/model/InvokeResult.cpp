/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/InvokeResult.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/HashingUtils.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Stream;
using namespace Aws::Utils;
using namespace Aws;

InvokeResult::InvokeResult() : 
    m_statusCode(0)
{
}

InvokeResult::InvokeResult(InvokeResult&& toMove) : 
    m_statusCode(toMove.m_statusCode),
    m_functionError(std::move(toMove.m_functionError)),
    m_logResult(std::move(toMove.m_logResult)),
    m_payload(std::move(toMove.m_payload)),
    m_executedVersion(std::move(toMove.m_executedVersion)),
    m_requestId(std::move(toMove.m_requestId))
{
}

InvokeResult& InvokeResult::operator=(InvokeResult&& toMove)
{
   if(this == &toMove)
   {
      return *this;
   }

   m_statusCode = toMove.m_statusCode;
   m_functionError = std::move(toMove.m_functionError);
   m_logResult = std::move(toMove.m_logResult);
   m_payload = std::move(toMove.m_payload);
   m_executedVersion = std::move(toMove.m_executedVersion);
   m_requestId = std::move(toMove.m_requestId);

   return *this;
}

InvokeResult::InvokeResult(Aws::AmazonWebServiceResult<ResponseStream>&& result)
  : InvokeResult()
{
  *this = std::move(result);
}

InvokeResult& InvokeResult::operator =(Aws::AmazonWebServiceResult<ResponseStream>&& result)
{
  m_payload = result.TakeOwnershipOfPayload();

  const auto& headers = result.GetHeaderValueCollection();
  const auto& functionErrorIter = headers.find("x-amz-function-error");
  if(functionErrorIter != headers.end())
  {
    m_functionError = functionErrorIter->second;
  }

  const auto& logResultIter = headers.find("x-amz-log-result");
  if(logResultIter != headers.end())
  {
    m_logResult = logResultIter->second;
  }

  const auto& executedVersionIter = headers.find("x-amz-executed-version");
  if(executedVersionIter != headers.end())
  {
    m_executedVersion = executedVersionIter->second;
  }

  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }

  m_statusCode = static_cast<int>(result.GetResponseCode());

   return *this;
}
