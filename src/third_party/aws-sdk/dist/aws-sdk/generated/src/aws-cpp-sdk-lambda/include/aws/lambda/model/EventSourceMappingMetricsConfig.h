/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/EventSourceMappingMetric.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{

  /**
   * <p>The metrics configuration for your event source. Use this configuration
   * object to define which metrics you want your event source mapping to
   * produce.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/EventSourceMappingMetricsConfig">AWS
   * API Reference</a></p>
   */
  class EventSourceMappingMetricsConfig
  {
  public:
    AWS_LAMBDA_API EventSourceMappingMetricsConfig();
    AWS_LAMBDA_API EventSourceMappingMetricsConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API EventSourceMappingMetricsConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p> The metrics you want your event source mapping to produce. Include
     * <code>EventCount</code> to receive event source mapping metrics related to the
     * number of events processed by your event source mapping. For more information
     * about these metrics, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/monitoring-metrics-types.html#event-source-mapping-metrics">
     * Event source mapping metrics</a>. </p>
     */
    inline const Aws::Vector<EventSourceMappingMetric>& GetMetrics() const{ return m_metrics; }
    inline bool MetricsHasBeenSet() const { return m_metricsHasBeenSet; }
    inline void SetMetrics(const Aws::Vector<EventSourceMappingMetric>& value) { m_metricsHasBeenSet = true; m_metrics = value; }
    inline void SetMetrics(Aws::Vector<EventSourceMappingMetric>&& value) { m_metricsHasBeenSet = true; m_metrics = std::move(value); }
    inline EventSourceMappingMetricsConfig& WithMetrics(const Aws::Vector<EventSourceMappingMetric>& value) { SetMetrics(value); return *this;}
    inline EventSourceMappingMetricsConfig& WithMetrics(Aws::Vector<EventSourceMappingMetric>&& value) { SetMetrics(std::move(value)); return *this;}
    inline EventSourceMappingMetricsConfig& AddMetrics(const EventSourceMappingMetric& value) { m_metricsHasBeenSet = true; m_metrics.push_back(value); return *this; }
    inline EventSourceMappingMetricsConfig& AddMetrics(EventSourceMappingMetric&& value) { m_metricsHasBeenSet = true; m_metrics.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::Vector<EventSourceMappingMetric> m_metrics;
    bool m_metricsHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
