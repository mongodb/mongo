/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/UpdateShardCountResult.h>
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

UpdateShardCountResult::UpdateShardCountResult() : 
    m_currentShardCount(0),
    m_targetShardCount(0)
{
}

UpdateShardCountResult::UpdateShardCountResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : UpdateShardCountResult()
{
  *this = result;
}

UpdateShardCountResult& UpdateShardCountResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("StreamName"))
  {
    m_streamName = jsonValue.GetString("StreamName");

  }

  if(jsonValue.ValueExists("CurrentShardCount"))
  {
    m_currentShardCount = jsonValue.GetInteger("CurrentShardCount");

  }

  if(jsonValue.ValueExists("TargetShardCount"))
  {
    m_targetShardCount = jsonValue.GetInteger("TargetShardCount");

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
