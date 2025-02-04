/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/model/Record.h>
#include <aws/kinesis/model/ChildShard.h>
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
   * <p>After you call <a>SubscribeToShard</a>, Kinesis Data Streams sends events of
   * this type over an HTTP/2 connection to your consumer.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/SubscribeToShardEvent">AWS
   * API Reference</a></p>
   */
  class SubscribeToShardEvent
  {
  public:
    AWS_KINESIS_API SubscribeToShardEvent();
    AWS_KINESIS_API SubscribeToShardEvent(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API SubscribeToShardEvent& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p/>
     */
    inline const Aws::Vector<Record>& GetRecords() const{ return m_records; }
    inline bool RecordsHasBeenSet() const { return m_recordsHasBeenSet; }
    inline void SetRecords(const Aws::Vector<Record>& value) { m_recordsHasBeenSet = true; m_records = value; }
    inline void SetRecords(Aws::Vector<Record>&& value) { m_recordsHasBeenSet = true; m_records = std::move(value); }
    inline SubscribeToShardEvent& WithRecords(const Aws::Vector<Record>& value) { SetRecords(value); return *this;}
    inline SubscribeToShardEvent& WithRecords(Aws::Vector<Record>&& value) { SetRecords(std::move(value)); return *this;}
    inline SubscribeToShardEvent& AddRecords(const Record& value) { m_recordsHasBeenSet = true; m_records.push_back(value); return *this; }
    inline SubscribeToShardEvent& AddRecords(Record&& value) { m_recordsHasBeenSet = true; m_records.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Use this as <code>SequenceNumber</code> in the next call to
     * <a>SubscribeToShard</a>, with <code>StartingPosition</code> set to
     * <code>AT_SEQUENCE_NUMBER</code> or <code>AFTER_SEQUENCE_NUMBER</code>. Use
     * <code>ContinuationSequenceNumber</code> for checkpointing because it captures
     * your shard progress even when no data is written to the shard.</p>
     */
    inline const Aws::String& GetContinuationSequenceNumber() const{ return m_continuationSequenceNumber; }
    inline bool ContinuationSequenceNumberHasBeenSet() const { return m_continuationSequenceNumberHasBeenSet; }
    inline void SetContinuationSequenceNumber(const Aws::String& value) { m_continuationSequenceNumberHasBeenSet = true; m_continuationSequenceNumber = value; }
    inline void SetContinuationSequenceNumber(Aws::String&& value) { m_continuationSequenceNumberHasBeenSet = true; m_continuationSequenceNumber = std::move(value); }
    inline void SetContinuationSequenceNumber(const char* value) { m_continuationSequenceNumberHasBeenSet = true; m_continuationSequenceNumber.assign(value); }
    inline SubscribeToShardEvent& WithContinuationSequenceNumber(const Aws::String& value) { SetContinuationSequenceNumber(value); return *this;}
    inline SubscribeToShardEvent& WithContinuationSequenceNumber(Aws::String&& value) { SetContinuationSequenceNumber(std::move(value)); return *this;}
    inline SubscribeToShardEvent& WithContinuationSequenceNumber(const char* value) { SetContinuationSequenceNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of milliseconds the read records are from the tip of the stream,
     * indicating how far behind current time the consumer is. A value of zero
     * indicates that record processing is caught up, and there are no new records to
     * process at this moment.</p>
     */
    inline long long GetMillisBehindLatest() const{ return m_millisBehindLatest; }
    inline bool MillisBehindLatestHasBeenSet() const { return m_millisBehindLatestHasBeenSet; }
    inline void SetMillisBehindLatest(long long value) { m_millisBehindLatestHasBeenSet = true; m_millisBehindLatest = value; }
    inline SubscribeToShardEvent& WithMillisBehindLatest(long long value) { SetMillisBehindLatest(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The list of the child shards of the current shard, returned only at the end
     * of the current shard.</p>
     */
    inline const Aws::Vector<ChildShard>& GetChildShards() const{ return m_childShards; }
    inline bool ChildShardsHasBeenSet() const { return m_childShardsHasBeenSet; }
    inline void SetChildShards(const Aws::Vector<ChildShard>& value) { m_childShardsHasBeenSet = true; m_childShards = value; }
    inline void SetChildShards(Aws::Vector<ChildShard>&& value) { m_childShardsHasBeenSet = true; m_childShards = std::move(value); }
    inline SubscribeToShardEvent& WithChildShards(const Aws::Vector<ChildShard>& value) { SetChildShards(value); return *this;}
    inline SubscribeToShardEvent& WithChildShards(Aws::Vector<ChildShard>&& value) { SetChildShards(std::move(value)); return *this;}
    inline SubscribeToShardEvent& AddChildShards(const ChildShard& value) { m_childShardsHasBeenSet = true; m_childShards.push_back(value); return *this; }
    inline SubscribeToShardEvent& AddChildShards(ChildShard&& value) { m_childShardsHasBeenSet = true; m_childShards.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::Vector<Record> m_records;
    bool m_recordsHasBeenSet = false;

    Aws::String m_continuationSequenceNumber;
    bool m_continuationSequenceNumberHasBeenSet = false;

    long long m_millisBehindLatest;
    bool m_millisBehindLatestHasBeenSet = false;

    Aws::Vector<ChildShard> m_childShards;
    bool m_childShardsHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
