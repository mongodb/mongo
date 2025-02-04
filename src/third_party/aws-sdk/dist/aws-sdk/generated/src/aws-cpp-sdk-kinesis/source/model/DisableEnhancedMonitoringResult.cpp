/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/DisableEnhancedMonitoringResult.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws;

DisableEnhancedMonitoringResult::DisableEnhancedMonitoringResult()
{
}

DisableEnhancedMonitoringResult::DisableEnhancedMonitoringResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

DisableEnhancedMonitoringResult& DisableEnhancedMonitoringResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("StreamName"))
  {
    m_streamName = jsonValue.GetString("StreamName");

  }

  if(jsonValue.ValueExists("CurrentShardLevelMetrics"))
  {
    Aws::Utils::Array<JsonView> currentShardLevelMetricsJsonList = jsonValue.GetArray("CurrentShardLevelMetrics");
    for(unsigned currentShardLevelMetricsIndex = 0; currentShardLevelMetricsIndex < currentShardLevelMetricsJsonList.GetLength(); ++currentShardLevelMetricsIndex)
    {
      m_currentShardLevelMetrics.push_back(MetricsNameMapper::GetMetricsNameForName(currentShardLevelMetricsJsonList[currentShardLevelMetricsIndex].AsString()));
    }
  }

  if(jsonValue.ValueExists("DesiredShardLevelMetrics"))
  {
    Aws::Utils::Array<JsonView> desiredShardLevelMetricsJsonList = jsonValue.GetArray("DesiredShardLevelMetrics");
    for(unsigned desiredShardLevelMetricsIndex = 0; desiredShardLevelMetricsIndex < desiredShardLevelMetricsJsonList.GetLength(); ++desiredShardLevelMetricsIndex)
    {
      m_desiredShardLevelMetrics.push_back(MetricsNameMapper::GetMetricsNameForName(desiredShardLevelMetricsJsonList[desiredShardLevelMetricsIndex].AsString()));
    }
  }

  if(jsonValue.ValueExists("StreamARN"))
  {
    m_streamARN = jsonValue.GetString("StreamARN");

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
