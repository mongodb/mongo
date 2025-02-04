/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/GetIdentityPoolRolesResult.h>
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

GetIdentityPoolRolesResult::GetIdentityPoolRolesResult()
{
}

GetIdentityPoolRolesResult::GetIdentityPoolRolesResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

GetIdentityPoolRolesResult& GetIdentityPoolRolesResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("IdentityPoolId"))
  {
    m_identityPoolId = jsonValue.GetString("IdentityPoolId");

  }

  if(jsonValue.ValueExists("Roles"))
  {
    Aws::Map<Aws::String, JsonView> rolesJsonMap = jsonValue.GetObject("Roles").GetAllObjects();
    for(auto& rolesItem : rolesJsonMap)
    {
      m_roles[rolesItem.first] = rolesItem.second.AsString();
    }
  }

  if(jsonValue.ValueExists("RoleMappings"))
  {
    Aws::Map<Aws::String, JsonView> roleMappingsJsonMap = jsonValue.GetObject("RoleMappings").GetAllObjects();
    for(auto& roleMappingsItem : roleMappingsJsonMap)
    {
      m_roleMappings[roleMappingsItem.first] = roleMappingsItem.second.AsObject();
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
