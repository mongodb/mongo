/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/MergeDeveloperIdentitiesRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

MergeDeveloperIdentitiesRequest::MergeDeveloperIdentitiesRequest() : 
    m_sourceUserIdentifierHasBeenSet(false),
    m_destinationUserIdentifierHasBeenSet(false),
    m_developerProviderNameHasBeenSet(false),
    m_identityPoolIdHasBeenSet(false)
{
}

Aws::String MergeDeveloperIdentitiesRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_sourceUserIdentifierHasBeenSet)
  {
   payload.WithString("SourceUserIdentifier", m_sourceUserIdentifier);

  }

  if(m_destinationUserIdentifierHasBeenSet)
  {
   payload.WithString("DestinationUserIdentifier", m_destinationUserIdentifier);

  }

  if(m_developerProviderNameHasBeenSet)
  {
   payload.WithString("DeveloperProviderName", m_developerProviderName);

  }

  if(m_identityPoolIdHasBeenSet)
  {
   payload.WithString("IdentityPoolId", m_identityPoolId);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection MergeDeveloperIdentitiesRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.MergeDeveloperIdentities"));
  return headers;

}




