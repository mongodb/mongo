/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/ListIdentityPoolsResult.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws;

ListIdentityPoolsResult::ListIdentityPoolsResult()
{
}

ListIdentityPoolsResult::ListIdentityPoolsResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

ListIdentityPoolsResult& ListIdentityPoolsResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("IdentityPools"))
  {
    Aws::Utils::Array<JsonView> identityPoolsJsonList = jsonValue.GetArray("IdentityPools");
    for(unsigned identityPoolsIndex = 0; identityPoolsIndex < identityPoolsJsonList.GetLength(); ++identityPoolsIndex)
    {
      m_identityPools.push_back(identityPoolsJsonList[identityPoolsIndex].AsObject());
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
