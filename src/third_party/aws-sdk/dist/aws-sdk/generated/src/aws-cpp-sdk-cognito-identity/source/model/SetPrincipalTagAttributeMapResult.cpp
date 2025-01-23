/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/SetPrincipalTagAttributeMapResult.h>
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

SetPrincipalTagAttributeMapResult::SetPrincipalTagAttributeMapResult() : 
    m_useDefaults(false)
{
}

SetPrincipalTagAttributeMapResult::SetPrincipalTagAttributeMapResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : SetPrincipalTagAttributeMapResult()
{
  *this = result;
}

SetPrincipalTagAttributeMapResult& SetPrincipalTagAttributeMapResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("IdentityPoolId"))
  {
    m_identityPoolId = jsonValue.GetString("IdentityPoolId");

  }

  if(jsonValue.ValueExists("IdentityProviderName"))
  {
    m_identityProviderName = jsonValue.GetString("IdentityProviderName");

  }

  if(jsonValue.ValueExists("UseDefaults"))
  {
    m_useDefaults = jsonValue.GetBool("UseDefaults");

  }

  if(jsonValue.ValueExists("PrincipalTags"))
  {
    Aws::Map<Aws::String, JsonView> principalTagsJsonMap = jsonValue.GetObject("PrincipalTags").GetAllObjects();
    for(auto& principalTagsItem : principalTagsJsonMap)
    {
      m_principalTags[principalTagsItem.first] = principalTagsItem.second.AsString();
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
