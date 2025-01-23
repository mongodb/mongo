/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/HashKeyRange.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Kinesis
{
namespace Model
{

HashKeyRange::HashKeyRange() : 
    m_startingHashKeyHasBeenSet(false),
    m_endingHashKeyHasBeenSet(false)
{
}

HashKeyRange::HashKeyRange(JsonView jsonValue)
  : HashKeyRange()
{
  *this = jsonValue;
}

HashKeyRange& HashKeyRange::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("StartingHashKey"))
  {
    m_startingHashKey = jsonValue.GetString("StartingHashKey");

    m_startingHashKeyHasBeenSet = true;
  }

  if(jsonValue.ValueExists("EndingHashKey"))
  {
    m_endingHashKey = jsonValue.GetString("EndingHashKey");

    m_endingHashKeyHasBeenSet = true;
  }

  return *this;
}

JsonValue HashKeyRange::Jsonize() const
{
  JsonValue payload;

  if(m_startingHashKeyHasBeenSet)
  {
   payload.WithString("StartingHashKey", m_startingHashKey);

  }

  if(m_endingHashKeyHasBeenSet)
  {
   payload.WithString("EndingHashKey", m_endingHashKey);

  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
