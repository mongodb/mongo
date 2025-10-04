/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/GetOpenIdTokenForDeveloperIdentityRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

GetOpenIdTokenForDeveloperIdentityRequest::GetOpenIdTokenForDeveloperIdentityRequest() : 
    m_identityPoolIdHasBeenSet(false),
    m_identityIdHasBeenSet(false),
    m_loginsHasBeenSet(false),
    m_principalTagsHasBeenSet(false),
    m_tokenDuration(0),
    m_tokenDurationHasBeenSet(false)
{
}

Aws::String GetOpenIdTokenForDeveloperIdentityRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_identityPoolIdHasBeenSet)
  {
   payload.WithString("IdentityPoolId", m_identityPoolId);

  }

  if(m_identityIdHasBeenSet)
  {
   payload.WithString("IdentityId", m_identityId);

  }

  if(m_loginsHasBeenSet)
  {
   JsonValue loginsJsonMap;
   for(auto& loginsItem : m_logins)
   {
     loginsJsonMap.WithString(loginsItem.first, loginsItem.second);
   }
   payload.WithObject("Logins", std::move(loginsJsonMap));

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

  if(m_tokenDurationHasBeenSet)
  {
   payload.WithInt64("TokenDuration", m_tokenDuration);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection GetOpenIdTokenForDeveloperIdentityRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.GetOpenIdTokenForDeveloperIdentity"));
  return headers;

}




