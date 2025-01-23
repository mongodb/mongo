/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/DeleteIdentitiesResult.h>
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

DeleteIdentitiesResult::DeleteIdentitiesResult()
{
}

DeleteIdentitiesResult::DeleteIdentitiesResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

DeleteIdentitiesResult& DeleteIdentitiesResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("UnprocessedIdentityIds"))
  {
    Aws::Utils::Array<JsonView> unprocessedIdentityIdsJsonList = jsonValue.GetArray("UnprocessedIdentityIds");
    for(unsigned unprocessedIdentityIdsIndex = 0; unprocessedIdentityIdsIndex < unprocessedIdentityIdsJsonList.GetLength(); ++unprocessedIdentityIdsIndex)
    {
      m_unprocessedIdentityIds.push_back(unprocessedIdentityIdsJsonList[unprocessedIdentityIdsIndex].AsObject());
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
