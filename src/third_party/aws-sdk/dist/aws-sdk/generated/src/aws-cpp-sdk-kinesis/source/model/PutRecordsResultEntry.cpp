/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/PutRecordsResultEntry.h>
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

PutRecordsResultEntry::PutRecordsResultEntry() : 
    m_sequenceNumberHasBeenSet(false),
    m_shardIdHasBeenSet(false),
    m_errorCodeHasBeenSet(false),
    m_errorMessageHasBeenSet(false)
{
}

PutRecordsResultEntry::PutRecordsResultEntry(JsonView jsonValue)
  : PutRecordsResultEntry()
{
  *this = jsonValue;
}

PutRecordsResultEntry& PutRecordsResultEntry::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("SequenceNumber"))
  {
    m_sequenceNumber = jsonValue.GetString("SequenceNumber");

    m_sequenceNumberHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ShardId"))
  {
    m_shardId = jsonValue.GetString("ShardId");

    m_shardIdHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ErrorCode"))
  {
    m_errorCode = jsonValue.GetString("ErrorCode");

    m_errorCodeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ErrorMessage"))
  {
    m_errorMessage = jsonValue.GetString("ErrorMessage");

    m_errorMessageHasBeenSet = true;
  }

  return *this;
}

JsonValue PutRecordsResultEntry::Jsonize() const
{
  JsonValue payload;

  if(m_sequenceNumberHasBeenSet)
  {
   payload.WithString("SequenceNumber", m_sequenceNumber);

  }

  if(m_shardIdHasBeenSet)
  {
   payload.WithString("ShardId", m_shardId);

  }

  if(m_errorCodeHasBeenSet)
  {
   payload.WithString("ErrorCode", m_errorCode);

  }

  if(m_errorMessageHasBeenSet)
  {
   payload.WithString("ErrorMessage", m_errorMessage);

  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
