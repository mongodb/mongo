/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/Array.h>
#include <aws/kinesis/model/EncryptionType.h>
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
   * <p>The unit of data of the Kinesis data stream, which is composed of a sequence
   * number, a partition key, and a data blob.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/Record">AWS API
   * Reference</a></p>
   */
  class Record
  {
  public:
    AWS_KINESIS_API Record();
    AWS_KINESIS_API Record(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Record& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The unique identifier of the record within its shard.</p>
     */
    inline const Aws::String& GetSequenceNumber() const{ return m_sequenceNumber; }
    inline bool SequenceNumberHasBeenSet() const { return m_sequenceNumberHasBeenSet; }
    inline void SetSequenceNumber(const Aws::String& value) { m_sequenceNumberHasBeenSet = true; m_sequenceNumber = value; }
    inline void SetSequenceNumber(Aws::String&& value) { m_sequenceNumberHasBeenSet = true; m_sequenceNumber = std::move(value); }
    inline void SetSequenceNumber(const char* value) { m_sequenceNumberHasBeenSet = true; m_sequenceNumber.assign(value); }
    inline Record& WithSequenceNumber(const Aws::String& value) { SetSequenceNumber(value); return *this;}
    inline Record& WithSequenceNumber(Aws::String&& value) { SetSequenceNumber(std::move(value)); return *this;}
    inline Record& WithSequenceNumber(const char* value) { SetSequenceNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The approximate time that the record was inserted into the stream.</p>
     */
    inline const Aws::Utils::DateTime& GetApproximateArrivalTimestamp() const{ return m_approximateArrivalTimestamp; }
    inline bool ApproximateArrivalTimestampHasBeenSet() const { return m_approximateArrivalTimestampHasBeenSet; }
    inline void SetApproximateArrivalTimestamp(const Aws::Utils::DateTime& value) { m_approximateArrivalTimestampHasBeenSet = true; m_approximateArrivalTimestamp = value; }
    inline void SetApproximateArrivalTimestamp(Aws::Utils::DateTime&& value) { m_approximateArrivalTimestampHasBeenSet = true; m_approximateArrivalTimestamp = std::move(value); }
    inline Record& WithApproximateArrivalTimestamp(const Aws::Utils::DateTime& value) { SetApproximateArrivalTimestamp(value); return *this;}
    inline Record& WithApproximateArrivalTimestamp(Aws::Utils::DateTime&& value) { SetApproximateArrivalTimestamp(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The data blob. The data in the blob is both opaque and immutable to Kinesis
     * Data Streams, which does not inspect, interpret, or change the data in the blob
     * in any way. When the data blob (the payload before base64-encoding) is added to
     * the partition key size, the total size must not exceed the maximum record size
     * (1 MiB).</p>
     */
    inline const Aws::Utils::ByteBuffer& GetData() const{ return m_data; }
    inline bool DataHasBeenSet() const { return m_dataHasBeenSet; }
    inline void SetData(const Aws::Utils::ByteBuffer& value) { m_dataHasBeenSet = true; m_data = value; }
    inline void SetData(Aws::Utils::ByteBuffer&& value) { m_dataHasBeenSet = true; m_data = std::move(value); }
    inline Record& WithData(const Aws::Utils::ByteBuffer& value) { SetData(value); return *this;}
    inline Record& WithData(Aws::Utils::ByteBuffer&& value) { SetData(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Identifies which shard in the stream the data record is assigned to.</p>
     */
    inline const Aws::String& GetPartitionKey() const{ return m_partitionKey; }
    inline bool PartitionKeyHasBeenSet() const { return m_partitionKeyHasBeenSet; }
    inline void SetPartitionKey(const Aws::String& value) { m_partitionKeyHasBeenSet = true; m_partitionKey = value; }
    inline void SetPartitionKey(Aws::String&& value) { m_partitionKeyHasBeenSet = true; m_partitionKey = std::move(value); }
    inline void SetPartitionKey(const char* value) { m_partitionKeyHasBeenSet = true; m_partitionKey.assign(value); }
    inline Record& WithPartitionKey(const Aws::String& value) { SetPartitionKey(value); return *this;}
    inline Record& WithPartitionKey(Aws::String&& value) { SetPartitionKey(std::move(value)); return *this;}
    inline Record& WithPartitionKey(const char* value) { SetPartitionKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The encryption type used on the record. This parameter can be one of the
     * following values:</p> <ul> <li> <p> <code>NONE</code>: Do not encrypt the
     * records in the stream.</p> </li> <li> <p> <code>KMS</code>: Use server-side
     * encryption on the records in the stream using a customer-managed Amazon Web
     * Services KMS key.</p> </li> </ul>
     */
    inline const EncryptionType& GetEncryptionType() const{ return m_encryptionType; }
    inline bool EncryptionTypeHasBeenSet() const { return m_encryptionTypeHasBeenSet; }
    inline void SetEncryptionType(const EncryptionType& value) { m_encryptionTypeHasBeenSet = true; m_encryptionType = value; }
    inline void SetEncryptionType(EncryptionType&& value) { m_encryptionTypeHasBeenSet = true; m_encryptionType = std::move(value); }
    inline Record& WithEncryptionType(const EncryptionType& value) { SetEncryptionType(value); return *this;}
    inline Record& WithEncryptionType(EncryptionType&& value) { SetEncryptionType(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_sequenceNumber;
    bool m_sequenceNumberHasBeenSet = false;

    Aws::Utils::DateTime m_approximateArrivalTimestamp;
    bool m_approximateArrivalTimestampHasBeenSet = false;

    Aws::Utils::ByteBuffer m_data;
    bool m_dataHasBeenSet = false;

    Aws::String m_partitionKey;
    bool m_partitionKeyHasBeenSet = false;

    EncryptionType m_encryptionType;
    bool m_encryptionTypeHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
