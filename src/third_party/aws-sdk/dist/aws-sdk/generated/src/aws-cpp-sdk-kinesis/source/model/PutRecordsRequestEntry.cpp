/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/PutRecordsRequestEntry.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/HashingUtils.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Kinesis
{
namespace Model
{

PutRecordsRequestEntry::PutRecordsRequestEntry() : 
    m_dataHasBeenSet(false),
    m_explicitHashKeyHasBeenSet(false),
    m_partitionKeyHasBeenSet(false)
{
}

PutRecordsRequestEntry::PutRecordsRequestEntry(JsonView jsonValue)
  : PutRecordsRequestEntry()
{
  *this = jsonValue;
}

PutRecordsRequestEntry& PutRecordsRequestEntry::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Data"))
  {
    m_data = HashingUtils::Base64Decode(jsonValue.GetString("Data"));
    m_dataHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ExplicitHashKey"))
  {
    m_explicitHashKey = jsonValue.GetString("ExplicitHashKey");

    m_explicitHashKeyHasBeenSet = true;
  }

  if(jsonValue.ValueExists("PartitionKey"))
  {
    m_partitionKey = jsonValue.GetString("PartitionKey");

    m_partitionKeyHasBeenSet = true;
  }

  return *this;
}

JsonValue PutRecordsRequestEntry::Jsonize() const
{
  JsonValue payload;

  if(m_dataHasBeenSet)
  {
   payload.WithString("Data", HashingUtils::Base64Encode(m_data));
  }

  if(m_explicitHashKeyHasBeenSet)
  {
   payload.WithString("ExplicitHashKey", m_explicitHashKey);

  }

  if(m_partitionKeyHasBeenSet)
  {
   payload.WithString("PartitionKey", m_partitionKey);

  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
