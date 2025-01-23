/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/DeleteIdentityPoolRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

DeleteIdentityPoolRequest::DeleteIdentityPoolRequest() : 
    m_identityPoolIdHasBeenSet(false)
{
}

Aws::String DeleteIdentityPoolRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_identityPoolIdHasBeenSet)
  {
   payload.WithString("IdentityPoolId", m_identityPoolId);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection DeleteIdentityPoolRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.DeleteIdentityPool"));
  return headers;

}




