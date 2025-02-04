/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/EventSourceMappingMetricsConfig.h>
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

EventSourceMappingMetricsConfig::EventSourceMappingMetricsConfig() : 
    m_metricsHasBeenSet(false)
{
}

EventSourceMappingMetricsConfig::EventSourceMappingMetricsConfig(JsonView jsonValue)
  : EventSourceMappingMetricsConfig()
{
  *this = jsonValue;
}

EventSourceMappingMetricsConfig& EventSourceMappingMetricsConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Metrics"))
  {
    Aws::Utils::Array<JsonView> metricsJsonList = jsonValue.GetArray("Metrics");
    for(unsigned metricsIndex = 0; metricsIndex < metricsJsonList.GetLength(); ++metricsIndex)
    {
      m_metrics.push_back(EventSourceMappingMetricMapper::GetEventSourceMappingMetricForName(metricsJsonList[metricsIndex].AsString()));
    }
    m_metricsHasBeenSet = true;
  }

  return *this;
}

JsonValue EventSourceMappingMetricsConfig::Jsonize() const
{
  JsonValue payload;

  if(m_metricsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> metricsJsonList(m_metrics.size());
   for(unsigned metricsIndex = 0; metricsIndex < metricsJsonList.GetLength(); ++metricsIndex)
   {
     metricsJsonList[metricsIndex].AsString(EventSourceMappingMetricMapper::GetNameForEventSourceMappingMetric(m_metrics[metricsIndex]));
   }
   payload.WithArray("Metrics", std::move(metricsJsonList));

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
