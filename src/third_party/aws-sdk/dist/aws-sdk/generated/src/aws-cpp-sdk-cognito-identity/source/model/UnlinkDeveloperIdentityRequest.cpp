/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/UnlinkDeveloperIdentityRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

UnlinkDeveloperIdentityRequest::UnlinkDeveloperIdentityRequest() : 
    m_identityIdHasBeenSet(false),
    m_identityPoolIdHasBeenSet(false),
    m_developerProviderNameHasBeenSet(false),
    m_developerUserIdentifierHasBeenSet(false)
{
}

Aws::String UnlinkDeveloperIdentityRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_identityIdHasBeenSet)
  {
   payload.WithString("IdentityId", m_identityId);

  }

  if(m_identityPoolIdHasBeenSet)
  {
   payload.WithString("IdentityPoolId", m_identityPoolId);

  }

  if(m_developerProviderNameHasBeenSet)
  {
   payload.WithString("DeveloperProviderName", m_developerProviderName);

  }

  if(m_developerUserIdentifierHasBeenSet)
  {
   payload.WithString("DeveloperUserIdentifier", m_developerUserIdentifier);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection UnlinkDeveloperIdentityRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.UnlinkDeveloperIdentity"));
  return headers;

}




