/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/kinesis/model/MetricsName.h>
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
namespace Kinesis
{
namespace Model
{

  /**
   * <p>Represents enhanced metrics types.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/EnhancedMetrics">AWS
   * API Reference</a></p>
   */
  class EnhancedMetrics
  {
  public:
    AWS_KINESIS_API EnhancedMetrics();
    AWS_KINESIS_API EnhancedMetrics(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API EnhancedMetrics& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>List of shard-level metrics.</p> <p>The following are the valid shard-level
     * metrics. The value "<code>ALL</code>" enhances every metric.</p> <ul> <li> <p>
     * <code>IncomingBytes</code> </p> </li> <li> <p> <code>IncomingRecords</code> </p>
     * </li> <li> <p> <code>OutgoingBytes</code> </p> </li> <li> <p>
     * <code>OutgoingRecords</code> </p> </li> <li> <p>
     * <code>WriteProvisionedThroughputExceeded</code> </p> </li> <li> <p>
     * <code>ReadProvisionedThroughputExceeded</code> </p> </li> <li> <p>
     * <code>IteratorAgeMilliseconds</code> </p> </li> <li> <p> <code>ALL</code> </p>
     * </li> </ul> <p>For more information, see <a
     * href="https://docs.aws.amazon.com/kinesis/latest/dev/monitoring-with-cloudwatch.html">Monitoring
     * the Amazon Kinesis Data Streams Service with Amazon CloudWatch</a> in the
     * <i>Amazon Kinesis Data Streams Developer Guide</i>.</p>
     */
    inline const Aws::Vector<MetricsName>& GetShardLevelMetrics() const{ return m_shardLevelMetrics; }
    inline bool ShardLevelMetricsHasBeenSet() const { return m_shardLevelMetricsHasBeenSet; }
    inline void SetShardLevelMetrics(const Aws::Vector<MetricsName>& value) { m_shardLevelMetricsHasBeenSet = true; m_shardLevelMetrics = value; }
    inline void SetShardLevelMetrics(Aws::Vector<MetricsName>&& value) { m_shardLevelMetricsHasBeenSet = true; m_shardLevelMetrics = std::move(value); }
    inline EnhancedMetrics& WithShardLevelMetrics(const Aws::Vector<MetricsName>& value) { SetShardLevelMetrics(value); return *this;}
    inline EnhancedMetrics& WithShardLevelMetrics(Aws::Vector<MetricsName>&& value) { SetShardLevelMetrics(std::move(value)); return *this;}
    inline EnhancedMetrics& AddShardLevelMetrics(const MetricsName& value) { m_shardLevelMetricsHasBeenSet = true; m_shardLevelMetrics.push_back(value); return *this; }
    inline EnhancedMetrics& AddShardLevelMetrics(MetricsName&& value) { m_shardLevelMetricsHasBeenSet = true; m_shardLevelMetrics.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::Vector<MetricsName> m_shardLevelMetrics;
    bool m_shardLevelMetricsHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
