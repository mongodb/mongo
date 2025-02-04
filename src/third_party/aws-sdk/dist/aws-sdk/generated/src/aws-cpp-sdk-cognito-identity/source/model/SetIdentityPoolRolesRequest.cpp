/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/SetIdentityPoolRolesRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

SetIdentityPoolRolesRequest::SetIdentityPoolRolesRequest() : 
    m_identityPoolIdHasBeenSet(false),
    m_rolesHasBeenSet(false),
    m_roleMappingsHasBeenSet(false)
{
}

Aws::String SetIdentityPoolRolesRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_identityPoolIdHasBeenSet)
  {
   payload.WithString("IdentityPoolId", m_identityPoolId);

  }

  if(m_rolesHasBeenSet)
  {
   JsonValue rolesJsonMap;
   for(auto& rolesItem : m_roles)
   {
     rolesJsonMap.WithString(rolesItem.first, rolesItem.second);
   }
   payload.WithObject("Roles", std::move(rolesJsonMap));

  }

  if(m_roleMappingsHasBeenSet)
  {
   JsonValue roleMappingsJsonMap;
   for(auto& roleMappingsItem : m_roleMappings)
   {
     roleMappingsJsonMap.WithObject(roleMappingsItem.first, roleMappingsItem.second.Jsonize());
   }
   payload.WithObject("RoleMappings", std::move(roleMappingsJsonMap));

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection SetIdentityPoolRolesRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.SetIdentityPoolRoles"));
  return headers;

}




