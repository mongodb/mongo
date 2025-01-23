/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/ChildShard.h>
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

ChildShard::ChildShard() : 
    m_shardIdHasBeenSet(false),
    m_parentShardsHasBeenSet(false),
    m_hashKeyRangeHasBeenSet(false)
{
}

ChildShard::ChildShard(JsonView jsonValue)
  : ChildShard()
{
  *this = jsonValue;
}

ChildShard& ChildShard::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ShardId"))
  {
    m_shardId = jsonValue.GetString("ShardId");

    m_shardIdHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ParentShards"))
  {
    Aws::Utils::Array<JsonView> parentShardsJsonList = jsonValue.GetArray("ParentShards");
    for(unsigned parentShardsIndex = 0; parentShardsIndex < parentShardsJsonList.GetLength(); ++parentShardsIndex)
    {
      m_parentShards.push_back(parentShardsJsonList[parentShardsIndex].AsString());
    }
    m_parentShardsHasBeenSet = true;
  }

  if(jsonValue.ValueExists("HashKeyRange"))
  {
    m_hashKeyRange = jsonValue.GetObject("HashKeyRange");

    m_hashKeyRangeHasBeenSet = true;
  }

  return *this;
}

JsonValue ChildShard::Jsonize() const
{
  JsonValue payload;

  if(m_shardIdHasBeenSet)
  {
   payload.WithString("ShardId", m_shardId);

  }

  if(m_parentShardsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> parentShardsJsonList(m_parentShards.size());
   for(unsigned parentShardsIndex = 0; parentShardsIndex < parentShardsJsonList.GetLength(); ++parentShardsIndex)
   {
     parentShardsJsonList[parentShardsIndex].AsString(m_parentShards[parentShardsIndex]);
   }
   payload.WithArray("ParentShards", std::move(parentShardsJsonList));

  }

  if(m_hashKeyRangeHasBeenSet)
  {
   payload.WithObject("HashKeyRange", m_hashKeyRange.Jsonize());

  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
