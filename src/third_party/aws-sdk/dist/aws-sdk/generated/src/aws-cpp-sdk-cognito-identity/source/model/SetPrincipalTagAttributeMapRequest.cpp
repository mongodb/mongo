/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/SetPrincipalTagAttributeMapRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

SetPrincipalTagAttributeMapRequest::SetPrincipalTagAttributeMapRequest() : 
    m_identityPoolIdHasBeenSet(false),
    m_identityProviderNameHasBeenSet(false),
    m_useDefaults(false),
    m_useDefaultsHasBeenSet(false),
    m_principalTagsHasBeenSet(false)
{
}

Aws::String SetPrincipalTagAttributeMapRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_identityPoolIdHasBeenSet)
  {
   payload.WithString("IdentityPoolId", m_identityPoolId);

  }

  if(m_identityProviderNameHasBeenSet)
  {
   payload.WithString("IdentityProviderName", m_identityProviderName);

  }

  if(m_useDefaultsHasBeenSet)
  {
   payload.WithBool("UseDefaults", m_useDefaults);

  }

  if(m_principalTagsHasBeenSet)
  {
   JsonValue principalTagsJsonMap;
   for(auto& principalTagsItem : m_principalTags)
   {
     principalTagsJsonMap.WithString(principalTagsItem.first, principalTagsItem.second);
   }
   payload.WithObject("PrincipalTags", std::move(principalTagsJsonMap));

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection SetPrincipalTagAttributeMapRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.SetPrincipalTagAttributeMap"));
  return headers;

}




