/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/EphemeralStorage.h>
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

EphemeralStorage::EphemeralStorage() : 
    m_size(0),
    m_sizeHasBeenSet(false)
{
}

EphemeralStorage::EphemeralStorage(JsonView jsonValue)
  : EphemeralStorage()
{
  *this = jsonValue;
}

EphemeralStorage& EphemeralStorage::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Size"))
  {
    m_size = jsonValue.GetInteger("Size");

    m_sizeHasBeenSet = true;
  }

  return *this;
}

JsonValue EphemeralStorage::Jsonize() const
{
  JsonValue payload;

  if(m_sizeHasBeenSet)
  {
   payload.WithInteger("Size", m_size);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
