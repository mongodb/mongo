/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/StartingPosition.h>
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

StartingPosition::StartingPosition() : 
    m_type(ShardIteratorType::NOT_SET),
    m_typeHasBeenSet(false),
    m_sequenceNumberHasBeenSet(false),
    m_timestampHasBeenSet(false)
{
}

StartingPosition::StartingPosition(JsonView jsonValue)
  : StartingPosition()
{
  *this = jsonValue;
}

StartingPosition& StartingPosition::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Type"))
  {
    m_type = ShardIteratorTypeMapper::GetShardIteratorTypeForName(jsonValue.GetString("Type"));

    m_typeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("SequenceNumber"))
  {
    m_sequenceNumber = jsonValue.GetString("SequenceNumber");

    m_sequenceNumberHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Timestamp"))
  {
    m_timestamp = jsonValue.GetDouble("Timestamp");

    m_timestampHasBeenSet = true;
  }

  return *this;
}

JsonValue StartingPosition::Jsonize() const
{
  JsonValue payload;

  if(m_typeHasBeenSet)
  {
   payload.WithString("Type", ShardIteratorTypeMapper::GetNameForShardIteratorType(m_type));
  }

  if(m_sequenceNumberHasBeenSet)
  {
   payload.WithString("SequenceNumber", m_sequenceNumber);

  }

  if(m_timestampHasBeenSet)
  {
   payload.WithDouble("Timestamp", m_timestamp.SecondsWithMSPrecision());
  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
