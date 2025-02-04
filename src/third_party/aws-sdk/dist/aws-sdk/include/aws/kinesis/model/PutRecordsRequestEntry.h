/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Represents the output for <code>PutRecords</code>.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/PutRecordsRequestEntry">AWS
   * API Reference</a></p>
   */
  class PutRecordsRequestEntry
  {
  public:
    AWS_KINESIS_API PutRecordsRequestEntry();
    AWS_KINESIS_API PutRecordsRequestEntry(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API PutRecordsRequestEntry& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The data blob to put into the record, which is base64-encoded when the blob
     * is serialized. When the data blob (the payload before base64-encoding) is added
     * to the partition key size, the total size must not exceed the maximum record
     * size (1 MiB).</p>
     */
    inline const Aws::Utils::ByteBuffer& GetData() const{ return m_data; }
    inline bool DataHasBeenSet() const { return m_dataHasBeenSet; }
    inline void SetData(const Aws::Utils::ByteBuffer& value) { m_dataHasBeenSet = true; m_data = value; }
    inline void SetData(Aws::Utils::ByteBuffer&& value) { m_dataHasBeenSet = true; m_data = std::move(value); }
    inline PutRecordsRequestEntry& WithData(const Aws::Utils::ByteBuffer& value) { SetData(value); return *this;}
    inline PutRecordsRequestEntry& WithData(Aws::Utils::ByteBuffer&& value) { SetData(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The hash value used to determine explicitly the shard that the data record is
     * assigned to by overriding the partition key hash.</p>
     */
    inline const Aws::String& GetExplicitHashKey() const{ return m_explicitHashKey; }
    inline bool ExplicitHashKeyHasBeenSet() const { return m_explicitHashKeyHasBeenSet; }
    inline void SetExplicitHashKey(const Aws::String& value) { m_explicitHashKeyHasBeenSet = true; m_explicitHashKey = value; }
    inline void SetExplicitHashKey(Aws::String&& value) { m_explicitHashKeyHasBeenSet = true; m_explicitHashKey = std::move(value); }
    inline void SetExplicitHashKey(const char* value) { m_explicitHashKeyHasBeenSet = true; m_explicitHashKey.assign(value); }
    inline PutRecordsRequestEntry& WithExplicitHashKey(const Aws::String& value) { SetExplicitHashKey(value); return *this;}
    inline PutRecordsRequestEntry& WithExplicitHashKey(Aws::String&& value) { SetExplicitHashKey(std::move(value)); return *this;}
    inline PutRecordsRequestEntry& WithExplicitHashKey(const char* value) { SetExplicitHashKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Determines which shard in the stream the data record is assigned to.
     * Partition keys are Unicode strings with a maximum length limit of 256 characters
     * for each key. Amazon Kinesis Data Streams uses the partition key as input to a
     * hash function that maps the partition key and associated data to a specific
     * shard. Specifically, an MD5 hash function is used to map partition keys to
     * 128-bit integer values and to map associated data records to shards. As a result
     * of this hashing mechanism, all data records with the same partition key map to
     * the same shard within the stream.</p>
     */
    inline const Aws::String& GetPartitionKey() const{ return m_partitionKey; }
    inline bool PartitionKeyHasBeenSet() const { return m_partitionKeyHasBeenSet; }
    inline void SetPartitionKey(const Aws::String& value) { m_partitionKeyHasBeenSet = true; m_partitionKey = value; }
    inline void SetPartitionKey(Aws::String&& value) { m_partitionKeyHasBeenSet = true; m_partitionKey = std::move(value); }
    inline void SetPartitionKey(const char* value) { m_partitionKeyHasBeenSet = true; m_partitionKey.assign(value); }
    inline PutRecordsRequestEntry& WithPartitionKey(const Aws::String& value) { SetPartitionKey(value); return *this;}
    inline PutRecordsRequestEntry& WithPartitionKey(Aws::String&& value) { SetPartitionKey(std::move(value)); return *this;}
    inline PutRecordsRequestEntry& WithPartitionKey(const char* value) { SetPartitionKey(value); return *this;}
    ///@}
  private:

    Aws::Utils::ByteBuffer m_data;
    bool m_dataHasBeenSet = false;

    Aws::String m_explicitHashKey;
    bool m_explicitHashKeyHasBeenSet = false;

    Aws::String m_partitionKey;
    bool m_partitionKeyHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
