/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/EnhancedMetrics.h>
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

EnhancedMetrics::EnhancedMetrics() : 
    m_shardLevelMetricsHasBeenSet(false)
{
}

EnhancedMetrics::EnhancedMetrics(JsonView jsonValue)
  : EnhancedMetrics()
{
  *this = jsonValue;
}

EnhancedMetrics& EnhancedMetrics::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ShardLevelMetrics"))
  {
    Aws::Utils::Array<JsonView> shardLevelMetricsJsonList = jsonValue.GetArray("ShardLevelMetrics");
    for(unsigned shardLevelMetricsIndex = 0; shardLevelMetricsIndex < shardLevelMetricsJsonList.GetLength(); ++shardLevelMetricsIndex)
    {
      m_shardLevelMetrics.push_back(MetricsNameMapper::GetMetricsNameForName(shardLevelMetricsJsonList[shardLevelMetricsIndex].AsString()));
    }
    m_shardLevelMetricsHasBeenSet = true;
  }

  return *this;
}

JsonValue EnhancedMetrics::Jsonize() const
{
  JsonValue payload;

  if(m_shardLevelMetricsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> shardLevelMetricsJsonList(m_shardLevelMetrics.size());
   for(unsigned shardLevelMetricsIndex = 0; shardLevelMetricsIndex < shardLevelMetricsJsonList.GetLength(); ++shardLevelMetricsIndex)
   {
     shardLevelMetricsJsonList[shardLevelMetricsIndex].AsString(MetricsNameMapper::GetNameForMetricsName(m_shardLevelMetrics[shardLevelMetricsIndex]));
   }
   payload.WithArray("ShardLevelMetrics", std::move(shardLevelMetricsJsonList));

  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
