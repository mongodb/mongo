/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/KinesisRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/Array.h>
#include <utility>

namespace Aws
{
namespace Kinesis
{
namespace Model
{

  /**
   * <p>Represents the input for <code>PutRecord</code>.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/PutRecordInput">AWS
   * API Reference</a></p>
   */
  class PutRecordRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API PutRecordRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PutRecord"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the stream to put the data record into.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline bool StreamNameHasBeenSet() const { return m_streamNameHasBeenSet; }
    inline void SetStreamName(const Aws::String& value) { m_streamNameHasBeenSet = true; m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamNameHasBeenSet = true; m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamNameHasBeenSet = true; m_streamName.assign(value); }
    inline PutRecordRequest& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline PutRecordRequest& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline PutRecordRequest& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

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
    inline PutRecordRequest& WithData(const Aws::Utils::ByteBuffer& value) { SetData(value); return *this;}
    inline PutRecordRequest& WithData(Aws::Utils::ByteBuffer&& value) { SetData(std::move(value)); return *this;}
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
    inline PutRecordRequest& WithPartitionKey(const Aws::String& value) { SetPartitionKey(value); return *this;}
    inline PutRecordRequest& WithPartitionKey(Aws::String&& value) { SetPartitionKey(std::move(value)); return *this;}
    inline PutRecordRequest& WithPartitionKey(const char* value) { SetPartitionKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The hash value used to explicitly determine the shard the data record is
     * assigned to by overriding the partition key hash.</p>
     */
    inline const Aws::String& GetExplicitHashKey() const{ return m_explicitHashKey; }
    inline bool ExplicitHashKeyHasBeenSet() const { return m_explicitHashKeyHasBeenSet; }
    inline void SetExplicitHashKey(const Aws::String& value) { m_explicitHashKeyHasBeenSet = true; m_explicitHashKey = value; }
    inline void SetExplicitHashKey(Aws::String&& value) { m_explicitHashKeyHasBeenSet = true; m_explicitHashKey = std::move(value); }
    inline void SetExplicitHashKey(const char* value) { m_explicitHashKeyHasBeenSet = true; m_explicitHashKey.assign(value); }
    inline PutRecordRequest& WithExplicitHashKey(const Aws::String& value) { SetExplicitHashKey(value); return *this;}
    inline PutRecordRequest& WithExplicitHashKey(Aws::String&& value) { SetExplicitHashKey(std::move(value)); return *this;}
    inline PutRecordRequest& WithExplicitHashKey(const char* value) { SetExplicitHashKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Guarantees strictly increasing sequence numbers, for puts from the same
     * client and to the same partition key. Usage: set the
     * <code>SequenceNumberForOrdering</code> of record <i>n</i> to the sequence number
     * of record <i>n-1</i> (as returned in the result when putting record <i>n-1</i>).
     * If this parameter is not set, records are coarsely ordered based on arrival
     * time.</p>
     */
    inline const Aws::String& GetSequenceNumberForOrdering() const{ return m_sequenceNumberForOrdering; }
    inline bool SequenceNumberForOrderingHasBeenSet() const { return m_sequenceNumberForOrderingHasBeenSet; }
    inline void SetSequenceNumberForOrdering(const Aws::String& value) { m_sequenceNumberForOrderingHasBeenSet = true; m_sequenceNumberForOrdering = value; }
    inline void SetSequenceNumberForOrdering(Aws::String&& value) { m_sequenceNumberForOrderingHasBeenSet = true; m_sequenceNumberForOrdering = std::move(value); }
    inline void SetSequenceNumberForOrdering(const char* value) { m_sequenceNumberForOrderingHasBeenSet = true; m_sequenceNumberForOrdering.assign(value); }
    inline PutRecordRequest& WithSequenceNumberForOrdering(const Aws::String& value) { SetSequenceNumberForOrdering(value); return *this;}
    inline PutRecordRequest& WithSequenceNumberForOrdering(Aws::String&& value) { SetSequenceNumberForOrdering(std::move(value)); return *this;}
    inline PutRecordRequest& WithSequenceNumberForOrdering(const char* value) { SetSequenceNumberForOrdering(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the stream.</p>
     */
    inline const Aws::String& GetStreamARN() const{ return m_streamARN; }
    inline bool StreamARNHasBeenSet() const { return m_streamARNHasBeenSet; }
    inline void SetStreamARN(const Aws::String& value) { m_streamARNHasBeenSet = true; m_streamARN = value; }
    inline void SetStreamARN(Aws::String&& value) { m_streamARNHasBeenSet = true; m_streamARN = std::move(value); }
    inline void SetStreamARN(const char* value) { m_streamARNHasBeenSet = true; m_streamARN.assign(value); }
    inline PutRecordRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline PutRecordRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline PutRecordRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;
    bool m_streamNameHasBeenSet = false;

    Aws::Utils::ByteBuffer m_data;
    bool m_dataHasBeenSet = false;

    Aws::String m_partitionKey;
    bool m_partitionKeyHasBeenSet = false;

    Aws::String m_explicitHashKey;
    bool m_explicitHashKeyHasBeenSet = false;

    Aws::String m_sequenceNumberForOrdering;
    bool m_sequenceNumberForOrderingHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
