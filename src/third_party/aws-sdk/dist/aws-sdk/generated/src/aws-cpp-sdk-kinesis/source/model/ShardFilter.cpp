/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/ShardFilter.h>
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

ShardFilter::ShardFilter() : 
    m_type(ShardFilterType::NOT_SET),
    m_typeHasBeenSet(false),
    m_shardIdHasBeenSet(false),
    m_timestampHasBeenSet(false)
{
}

ShardFilter::ShardFilter(JsonView jsonValue)
  : ShardFilter()
{
  *this = jsonValue;
}

ShardFilter& ShardFilter::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Type"))
  {
    m_type = ShardFilterTypeMapper::GetShardFilterTypeForName(jsonValue.GetString("Type"));

    m_typeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ShardId"))
  {
    m_shardId = jsonValue.GetString("ShardId");

    m_shardIdHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Timestamp"))
  {
    m_timestamp = jsonValue.GetDouble("Timestamp");

    m_timestampHasBeenSet = true;
  }

  return *this;
}

JsonValue ShardFilter::Jsonize() const
{
  JsonValue payload;

  if(m_typeHasBeenSet)
  {
   payload.WithString("Type", ShardFilterTypeMapper::GetNameForShardFilterType(m_type));
  }

  if(m_shardIdHasBeenSet)
  {
   payload.WithString("ShardId", m_shardId);

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
