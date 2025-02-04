/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/OnSuccess.h>
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

OnSuccess::OnSuccess() : 
    m_destinationHasBeenSet(false)
{
}

OnSuccess::OnSuccess(JsonView jsonValue)
  : OnSuccess()
{
  *this = jsonValue;
}

OnSuccess& OnSuccess::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Destination"))
  {
    m_destination = jsonValue.GetString("Destination");

    m_destinationHasBeenSet = true;
  }

  return *this;
}

JsonValue OnSuccess::Jsonize() const
{
  JsonValue payload;

  if(m_destinationHasBeenSet)
  {
   payload.WithString("Destination", m_destination);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
