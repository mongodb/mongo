/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/IdentityPoolShortDescription.h>
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

IdentityPoolShortDescription::IdentityPoolShortDescription() : 
    m_identityPoolIdHasBeenSet(false),
    m_identityPoolNameHasBeenSet(false)
{
}

IdentityPoolShortDescription::IdentityPoolShortDescription(JsonView jsonValue)
  : IdentityPoolShortDescription()
{
  *this = jsonValue;
}

IdentityPoolShortDescription& IdentityPoolShortDescription::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("IdentityPoolId"))
  {
    m_identityPoolId = jsonValue.GetString("IdentityPoolId");

    m_identityPoolIdHasBeenSet = true;
  }

  if(jsonValue.ValueExists("IdentityPoolName"))
  {
    m_identityPoolName = jsonValue.GetString("IdentityPoolName");

    m_identityPoolNameHasBeenSet = true;
  }

  return *this;
}

JsonValue IdentityPoolShortDescription::Jsonize() const
{
  JsonValue payload;

  if(m_identityPoolIdHasBeenSet)
  {
   payload.WithString("IdentityPoolId", m_identityPoolId);

  }

  if(m_identityPoolNameHasBeenSet)
  {
   payload.WithString("IdentityPoolName", m_identityPoolName);

  }

  return payload;
}

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
