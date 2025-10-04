/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/IdentityDescription.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

IdentityDescription::IdentityDescription() : 
    m_identityIdHasBeenSet(false),
    m_loginsHasBeenSet(false),
    m_creationDateHasBeenSet(false),
    m_lastModifiedDateHasBeenSet(false),
    m_requestIdHasBeenSet(false)
{
}

IdentityDescription::IdentityDescription(JsonView jsonValue)
  : IdentityDescription()
{
  *this = jsonValue;
}

IdentityDescription& IdentityDescription::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("IdentityId"))
  {
    m_identityId = jsonValue.GetString("IdentityId");

    m_identityIdHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Logins"))
  {
    Aws::Utils::Array<JsonView> loginsJsonList = jsonValue.GetArray("Logins");
    for(unsigned loginsIndex = 0; loginsIndex < loginsJsonList.GetLength(); ++loginsIndex)
    {
      m_logins.push_back(loginsJsonList[loginsIndex].AsString());
    }
    m_loginsHasBeenSet = true;
  }

  if(jsonValue.ValueExists("CreationDate"))
  {
    m_creationDate = jsonValue.GetDouble("CreationDate");

    m_creationDateHasBeenSet = true;
  }

  if(jsonValue.ValueExists("LastModifiedDate"))
  {
    m_lastModifiedDate = jsonValue.GetDouble("LastModifiedDate");

    m_lastModifiedDateHasBeenSet = true;
  }

  return *this;
}

JsonValue IdentityDescription::Jsonize() const
{
  JsonValue payload;

  if(m_identityIdHasBeenSet)
  {
   payload.WithString("IdentityId", m_identityId);

  }

  if(m_loginsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> loginsJsonList(m_logins.size());
   for(unsigned loginsIndex = 0; loginsIndex < loginsJsonList.GetLength(); ++loginsIndex)
   {
     loginsJsonList[loginsIndex].AsString(m_logins[loginsIndex]);
   }
   payload.WithArray("Logins", std::move(loginsJsonList));

  }

  if(m_creationDateHasBeenSet)
  {
   payload.WithDouble("CreationDate", m_creationDate.SecondsWithMSPrecision());
  }

  if(m_lastModifiedDateHasBeenSet)
  {
   payload.WithDouble("LastModifiedDate", m_lastModifiedDate.SecondsWithMSPrecision());
  }

  return payload;
}

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
