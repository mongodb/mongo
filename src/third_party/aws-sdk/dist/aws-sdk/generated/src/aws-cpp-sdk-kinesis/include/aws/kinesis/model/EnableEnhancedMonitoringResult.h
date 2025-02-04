/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/kinesis/model/MetricsName.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Kinesis
{
namespace Model
{
  /**
   * <p>Represents the output for <a>EnableEnhancedMonitoring</a> and
   * <a>DisableEnhancedMonitoring</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/EnhancedMonitoringOutput">AWS
   * API Reference</a></p>
   */
  class EnableEnhancedMonitoringResult
  {
  public:
    AWS_KINESIS_API EnableEnhancedMonitoringResult();
    AWS_KINESIS_API EnableEnhancedMonitoringResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API EnableEnhancedMonitoringResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The name of the Kinesis data stream.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline void SetStreamName(const Aws::String& value) { m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamName.assign(value); }
    inline EnableEnhancedMonitoringResult& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline EnableEnhancedMonitoringResult& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline EnableEnhancedMonitoringResult& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Represents the current state of the metrics that are in the enhanced state
     * before the operation.</p>
     */
    inline const Aws::Vector<MetricsName>& GetCurrentShardLevelMetrics() const{ return m_currentShardLevelMetrics; }
    inline void SetCurrentShardLevelMetrics(const Aws::Vector<MetricsName>& value) { m_currentShardLevelMetrics = value; }
    inline void SetCurrentShardLevelMetrics(Aws::Vector<MetricsName>&& value) { m_currentShardLevelMetrics = std::move(value); }
    inline EnableEnhancedMonitoringResult& WithCurrentShardLevelMetrics(const Aws::Vector<MetricsName>& value) { SetCurrentShardLevelMetrics(value); return *this;}
    inline EnableEnhancedMonitoringResult& WithCurrentShardLevelMetrics(Aws::Vector<MetricsName>&& value) { SetCurrentShardLevelMetrics(std::move(value)); return *this;}
    inline EnableEnhancedMonitoringResult& AddCurrentShardLevelMetrics(const MetricsName& value) { m_currentShardLevelMetrics.push_back(value); return *this; }
    inline EnableEnhancedMonitoringResult& AddCurrentShardLevelMetrics(MetricsName&& value) { m_currentShardLevelMetrics.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Represents the list of all the metrics that would be in the enhanced state
     * after the operation.</p>
     */
    inline const Aws::Vector<MetricsName>& GetDesiredShardLevelMetrics() const{ return m_desiredShardLevelMetrics; }
    inline void SetDesiredShardLevelMetrics(const Aws::Vector<MetricsName>& value) { m_desiredShardLevelMetrics = value; }
    inline void SetDesiredShardLevelMetrics(Aws::Vector<MetricsName>&& value) { m_desiredShardLevelMetrics = std::move(value); }
    inline EnableEnhancedMonitoringResult& WithDesiredShardLevelMetrics(const Aws::Vector<MetricsName>& value) { SetDesiredShardLevelMetrics(value); return *this;}
    inline EnableEnhancedMonitoringResult& WithDesiredShardLevelMetrics(Aws::Vector<MetricsName>&& value) { SetDesiredShardLevelMetrics(std::move(value)); return *this;}
    inline EnableEnhancedMonitoringResult& AddDesiredShardLevelMetrics(const MetricsName& value) { m_desiredShardLevelMetrics.push_back(value); return *this; }
    inline EnableEnhancedMonitoringResult& AddDesiredShardLevelMetrics(MetricsName&& value) { m_desiredShardLevelMetrics.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The ARN of the stream.</p>
     */
    inline const Aws::String& GetStreamARN() const{ return m_streamARN; }
    inline void SetStreamARN(const Aws::String& value) { m_streamARN = value; }
    inline void SetStreamARN(Aws::String&& value) { m_streamARN = std::move(value); }
    inline void SetStreamARN(const char* value) { m_streamARN.assign(value); }
    inline EnableEnhancedMonitoringResult& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline EnableEnhancedMonitoringResult& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline EnableEnhancedMonitoringResult& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline EnableEnhancedMonitoringResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline EnableEnhancedMonitoringResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline EnableEnhancedMonitoringResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;

    Aws::Vector<MetricsName> m_currentShardLevelMetrics;

    Aws::Vector<MetricsName> m_desiredShardLevelMetrics;

    Aws::String m_streamARN;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
