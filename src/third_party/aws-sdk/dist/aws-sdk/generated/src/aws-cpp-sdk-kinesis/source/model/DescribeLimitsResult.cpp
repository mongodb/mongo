/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/DescribeLimitsResult.h>
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

DescribeLimitsResult::DescribeLimitsResult() : 
    m_shardLimit(0),
    m_openShardCount(0),
    m_onDemandStreamCount(0),
    m_onDemandStreamCountLimit(0)
{
}

DescribeLimitsResult::DescribeLimitsResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : DescribeLimitsResult()
{
  *this = result;
}

DescribeLimitsResult& DescribeLimitsResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("ShardLimit"))
  {
    m_shardLimit = jsonValue.GetInteger("ShardLimit");

  }

  if(jsonValue.ValueExists("OpenShardCount"))
  {
    m_openShardCount = jsonValue.GetInteger("OpenShardCount");

  }

  if(jsonValue.ValueExists("OnDemandStreamCount"))
  {
    m_onDemandStreamCount = jsonValue.GetInteger("OnDemandStreamCount");

  }

  if(jsonValue.ValueExists("OnDemandStreamCountLimit"))
  {
    m_onDemandStreamCountLimit = jsonValue.GetInteger("OnDemandStreamCountLimit");

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
