/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/GetPrincipalTagAttributeMapRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

GetPrincipalTagAttributeMapRequest::GetPrincipalTagAttributeMapRequest() : 
    m_identityPoolIdHasBeenSet(false),
    m_identityProviderNameHasBeenSet(false)
{
}

Aws::String GetPrincipalTagAttributeMapRequest::SerializePayload() const
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

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection GetPrincipalTagAttributeMapRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.GetPrincipalTagAttributeMap"));
  return headers;

}




