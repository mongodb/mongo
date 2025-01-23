/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/AllowedPublishers.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

AllowedPublishers::AllowedPublishers() : 
    m_signingProfileVersionArnsHasBeenSet(false)
{
}

AllowedPublishers::AllowedPublishers(JsonView jsonValue)
  : AllowedPublishers()
{
  *this = jsonValue;
}

AllowedPublishers& AllowedPublishers::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("SigningProfileVersionArns"))
  {
    Aws::Utils::Array<JsonView> signingProfileVersionArnsJsonList = jsonValue.GetArray("SigningProfileVersionArns");
    for(unsigned signingProfileVersionArnsIndex = 0; signingProfileVersionArnsIndex < signingProfileVersionArnsJsonList.GetLength(); ++signingProfileVersionArnsIndex)
    {
      m_signingProfileVersionArns.push_back(signingProfileVersionArnsJsonList[signingProfileVersionArnsIndex].AsString());
    }
    m_signingProfileVersionArnsHasBeenSet = true;
  }

  return *this;
}

JsonValue AllowedPublishers::Jsonize() const
{
  JsonValue payload;

  if(m_signingProfileVersionArnsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> signingProfileVersionArnsJsonList(m_signingProfileVersionArns.size());
   for(unsigned signingProfileVersionArnsIndex = 0; signingProfileVersionArnsIndex < signingProfileVersionArnsJsonList.GetLength(); ++signingProfileVersionArnsIndex)
   {
     signingProfileVersionArnsJsonList[signingProfileVersionArnsIndex].AsString(m_signingProfileVersionArns[signingProfileVersionArnsIndex]);
   }
   payload.WithArray("SigningProfileVersionArns", std::move(signingProfileVersionArnsJsonList));

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
