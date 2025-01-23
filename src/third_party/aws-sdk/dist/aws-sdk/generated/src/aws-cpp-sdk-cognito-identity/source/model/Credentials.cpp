/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/Credentials.h>
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

Credentials::Credentials() : 
    m_accessKeyIdHasBeenSet(false),
    m_secretKeyHasBeenSet(false),
    m_sessionTokenHasBeenSet(false),
    m_expirationHasBeenSet(false)
{
}

Credentials::Credentials(JsonView jsonValue)
  : Credentials()
{
  *this = jsonValue;
}

Credentials& Credentials::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("AccessKeyId"))
  {
    m_accessKeyId = jsonValue.GetString("AccessKeyId");

    m_accessKeyIdHasBeenSet = true;
  }

  if(jsonValue.ValueExists("SecretKey"))
  {
    m_secretKey = jsonValue.GetString("SecretKey");

    m_secretKeyHasBeenSet = true;
  }

  if(jsonValue.ValueExists("SessionToken"))
  {
    m_sessionToken = jsonValue.GetString("SessionToken");

    m_sessionTokenHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Expiration"))
  {
    m_expiration = jsonValue.GetDouble("Expiration");

    m_expirationHasBeenSet = true;
  }

  return *this;
}

JsonValue Credentials::Jsonize() const
{
  JsonValue payload;

  if(m_accessKeyIdHasBeenSet)
  {
   payload.WithString("AccessKeyId", m_accessKeyId);

  }

  if(m_secretKeyHasBeenSet)
  {
   payload.WithString("SecretKey", m_secretKey);

  }

  if(m_sessionTokenHasBeenSet)
  {
   payload.WithString("SessionToken", m_sessionToken);

  }

  if(m_expirationHasBeenSet)
  {
   payload.WithDouble("Expiration", m_expiration.SecondsWithMSPrecision());
  }

  return payload;
}

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
