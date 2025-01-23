/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/CreateIdentityPoolRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::CognitoIdentity::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

CreateIdentityPoolRequest::CreateIdentityPoolRequest() : 
    m_identityPoolNameHasBeenSet(false),
    m_allowUnauthenticatedIdentities(false),
    m_allowUnauthenticatedIdentitiesHasBeenSet(false),
    m_allowClassicFlow(false),
    m_allowClassicFlowHasBeenSet(false),
    m_supportedLoginProvidersHasBeenSet(false),
    m_developerProviderNameHasBeenSet(false),
    m_openIdConnectProviderARNsHasBeenSet(false),
    m_cognitoIdentityProvidersHasBeenSet(false),
    m_samlProviderARNsHasBeenSet(false),
    m_identityPoolTagsHasBeenSet(false)
{
}

Aws::String CreateIdentityPoolRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_identityPoolNameHasBeenSet)
  {
   payload.WithString("IdentityPoolName", m_identityPoolName);

  }

  if(m_allowUnauthenticatedIdentitiesHasBeenSet)
  {
   payload.WithBool("AllowUnauthenticatedIdentities", m_allowUnauthenticatedIdentities);

  }

  if(m_allowClassicFlowHasBeenSet)
  {
   payload.WithBool("AllowClassicFlow", m_allowClassicFlow);

  }

  if(m_supportedLoginProvidersHasBeenSet)
  {
   JsonValue supportedLoginProvidersJsonMap;
   for(auto& supportedLoginProvidersItem : m_supportedLoginProviders)
   {
     supportedLoginProvidersJsonMap.WithString(supportedLoginProvidersItem.first, supportedLoginProvidersItem.second);
   }
   payload.WithObject("SupportedLoginProviders", std::move(supportedLoginProvidersJsonMap));

  }

  if(m_developerProviderNameHasBeenSet)
  {
   payload.WithString("DeveloperProviderName", m_developerProviderName);

  }

  if(m_openIdConnectProviderARNsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> openIdConnectProviderARNsJsonList(m_openIdConnectProviderARNs.size());
   for(unsigned openIdConnectProviderARNsIndex = 0; openIdConnectProviderARNsIndex < openIdConnectProviderARNsJsonList.GetLength(); ++openIdConnectProviderARNsIndex)
   {
     openIdConnectProviderARNsJsonList[openIdConnectProviderARNsIndex].AsString(m_openIdConnectProviderARNs[openIdConnectProviderARNsIndex]);
   }
   payload.WithArray("OpenIdConnectProviderARNs", std::move(openIdConnectProviderARNsJsonList));

  }

  if(m_cognitoIdentityProvidersHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> cognitoIdentityProvidersJsonList(m_cognitoIdentityProviders.size());
   for(unsigned cognitoIdentityProvidersIndex = 0; cognitoIdentityProvidersIndex < cognitoIdentityProvidersJsonList.GetLength(); ++cognitoIdentityProvidersIndex)
   {
     cognitoIdentityProvidersJsonList[cognitoIdentityProvidersIndex].AsObject(m_cognitoIdentityProviders[cognitoIdentityProvidersIndex].Jsonize());
   }
   payload.WithArray("CognitoIdentityProviders", std::move(cognitoIdentityProvidersJsonList));

  }

  if(m_samlProviderARNsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> samlProviderARNsJsonList(m_samlProviderARNs.size());
   for(unsigned samlProviderARNsIndex = 0; samlProviderARNsIndex < samlProviderARNsJsonList.GetLength(); ++samlProviderARNsIndex)
   {
     samlProviderARNsJsonList[samlProviderARNsIndex].AsString(m_samlProviderARNs[samlProviderARNsIndex]);
   }
   payload.WithArray("SamlProviderARNs", std::move(samlProviderARNsJsonList));

  }

  if(m_identityPoolTagsHasBeenSet)
  {
   JsonValue identityPoolTagsJsonMap;
   for(auto& identityPoolTagsItem : m_identityPoolTags)
   {
     identityPoolTagsJsonMap.WithString(identityPoolTagsItem.first, identityPoolTagsItem.second);
   }
   payload.WithObject("IdentityPoolTags", std::move(identityPoolTagsJsonMap));

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection CreateIdentityPoolRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "AWSCognitoIdentityService.CreateIdentityPool"));
  return headers;

}




