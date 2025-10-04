/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/SequenceNumberRange.h>
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

SequenceNumberRange::SequenceNumberRange() : 
    m_startingSequenceNumberHasBeenSet(false),
    m_endingSequenceNumberHasBeenSet(false)
{
}

SequenceNumberRange::SequenceNumberRange(JsonView jsonValue)
  : SequenceNumberRange()
{
  *this = jsonValue;
}

SequenceNumberRange& SequenceNumberRange::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("StartingSequenceNumber"))
  {
    m_startingSequenceNumber = jsonValue.GetString("StartingSequenceNumber");

    m_startingSequenceNumberHasBeenSet = true;
  }

  if(jsonValue.ValueExists("EndingSequenceNumber"))
  {
    m_endingSequenceNumber = jsonValue.GetString("EndingSequenceNumber");

    m_endingSequenceNumberHasBeenSet = true;
  }

  return *this;
}

JsonValue SequenceNumberRange::Jsonize() const
{
  JsonValue payload;

  if(m_startingSequenceNumberHasBeenSet)
  {
   payload.WithString("StartingSequenceNumber", m_startingSequenceNumber);

  }

  if(m_endingSequenceNumberHasBeenSet)
  {
   payload.WithString("EndingSequenceNumber", m_endingSequenceNumber);

  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
