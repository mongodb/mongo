/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/CognitoIdentityProvider.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

CognitoIdentityProvider::CognitoIdentityProvider() : 
    m_providerNameHasBeenSet(false),
    m_clientIdHasBeenSet(false),
    m_serverSideTokenCheck(false),
    m_serverSideTokenCheckHasBeenSet(false)
{
}

CognitoIdentityProvider::CognitoIdentityProvider(JsonView jsonValue)
  : CognitoIdentityProvider()
{
  *this = jsonValue;
}

CognitoIdentityProvider& CognitoIdentityProvider::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ProviderName"))
  {
    m_providerName = jsonValue.GetString("ProviderName");

    m_providerNameHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ClientId"))
  {
    m_clientId = jsonValue.GetString("ClientId");

    m_clientIdHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ServerSideTokenCheck"))
  {
    m_serverSideTokenCheck = jsonValue.GetBool("ServerSideTokenCheck");

    m_serverSideTokenCheckHasBeenSet = true;
  }

  return *this;
}

JsonValue CognitoIdentityProvider::Jsonize() const
{
  JsonValue payload;

  if(m_providerNameHasBeenSet)
  {
   payload.WithString("ProviderName", m_providerName);

  }

  if(m_clientIdHasBeenSet)
  {
   payload.WithString("ClientId", m_clientId);

  }

  if(m_serverSideTokenCheckHasBeenSet)
  {
   payload.WithBool("ServerSideTokenCheck", m_serverSideTokenCheck);

  }

  return payload;
}

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
