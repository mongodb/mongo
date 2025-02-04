/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/DeleteIdentitiesRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

DeleteIdentitiesRequest::DeleteIdentitiesRequest() : 
    m_identityIdsToDeleteHasBeenSet(false)
{
}

Aws::String DeleteIdentitiesRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_identityIdsToDeleteHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> identityIdsToDeleteJsonList(m_identityIdsToDelete.size());
   for(unsigned identityIdsToDeleteIndex = 0; identityIdsToDeleteIndex < identityIdsToDeleteJsonList.GetLength(); ++identityIdsToDeleteIndex)
   {
     identityIdsToDeleteJsonList[identityIdsToDeleteIndex].AsString(m_identityIdsToDelete[identityIdsToDeleteIndex]);
   }
   payload.WithArray("IdentityIdsToDelete", std::move(identityIdsToDeleteJsonList));

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection DeleteIdentitiesRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.DeleteIdentities"));
  return headers;

}




