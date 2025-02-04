/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/Record.h>
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

Record::Record() : 
    m_sequenceNumberHasBeenSet(false),
    m_approximateArrivalTimestampHasBeenSet(false),
    m_dataHasBeenSet(false),
    m_partitionKeyHasBeenSet(false),
    m_encryptionType(EncryptionType::NOT_SET),
    m_encryptionTypeHasBeenSet(false)
{
}

Record::Record(JsonView jsonValue)
  : Record()
{
  *this = jsonValue;
}

Record& Record::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("SequenceNumber"))
  {
    m_sequenceNumber = jsonValue.GetString("SequenceNumber");

    m_sequenceNumberHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ApproximateArrivalTimestamp"))
  {
    m_approximateArrivalTimestamp = jsonValue.GetDouble("ApproximateArrivalTimestamp");

    m_approximateArrivalTimestampHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Data"))
  {
    m_data = HashingUtils::Base64Decode(jsonValue.GetString("Data"));
    m_dataHasBeenSet = true;
  }

  if(jsonValue.ValueExists("PartitionKey"))
  {
    m_partitionKey = jsonValue.GetString("PartitionKey");

    m_partitionKeyHasBeenSet = true;
  }

  if(jsonValue.ValueExists("EncryptionType"))
  {
    m_encryptionType = EncryptionTypeMapper::GetEncryptionTypeForName(jsonValue.GetString("EncryptionType"));

    m_encryptionTypeHasBeenSet = true;
  }

  return *this;
}

JsonValue Record::Jsonize() const
{
  JsonValue payload;

  if(m_sequenceNumberHasBeenSet)
  {
   payload.WithString("SequenceNumber", m_sequenceNumber);

  }

  if(m_approximateArrivalTimestampHasBeenSet)
  {
   payload.WithDouble("ApproximateArrivalTimestamp", m_approximateArrivalTimestamp.SecondsWithMSPrecision());
  }

  if(m_dataHasBeenSet)
  {
   payload.WithString("Data", HashingUtils::Base64Encode(m_data));
  }

  if(m_partitionKeyHasBeenSet)
  {
   payload.WithString("PartitionKey", m_partitionKey);

  }

  if(m_encryptionTypeHasBeenSet)
  {
   payload.WithString("EncryptionType", EncryptionTypeMapper::GetNameForEncryptionType(m_encryptionType));
  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
