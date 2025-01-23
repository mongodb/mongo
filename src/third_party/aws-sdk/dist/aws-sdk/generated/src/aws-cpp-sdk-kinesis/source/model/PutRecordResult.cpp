/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/PutRecordResult.h>
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

PutRecordResult::PutRecordResult() : 
    m_encryptionType(EncryptionType::NOT_SET)
{
}

PutRecordResult::PutRecordResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : PutRecordResult()
{
  *this = result;
}

PutRecordResult& PutRecordResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("ShardId"))
  {
    m_shardId = jsonValue.GetString("ShardId");

  }

  if(jsonValue.ValueExists("SequenceNumber"))
  {
    m_sequenceNumber = jsonValue.GetString("SequenceNumber");

  }

  if(jsonValue.ValueExists("EncryptionType"))
  {
    m_encryptionType = EncryptionTypeMapper::GetEncryptionTypeForName(jsonValue.GetString("EncryptionType"));

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
