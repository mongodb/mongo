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
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Kinesis
{
namespace Model
{
  /**
   * <p>Represents the output for <a>GetRecords</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/GetRecordsOutput">AWS
   * API Reference</a></p>
   */
  class GetRecordsResult
  {
  public:
    AWS_KINESIS_API GetRecordsResult();
    AWS_KINESIS_API GetRecordsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API GetRecordsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The data records retrieved from the shard.</p>
     */
    inline const Aws::Vector<Record>& GetRecords() const{ return m_records; }
    inline void SetRecords(const Aws::Vector<Record>& value) { m_records = value; }
    inline void SetRecords(Aws::Vector<Record>&& value) { m_records = std::move(value); }
    inline GetRecordsResult& WithRecords(const Aws::Vector<Record>& value) { SetRecords(value); return *this;}
    inline GetRecordsResult& WithRecords(Aws::Vector<Record>&& value) { SetRecords(std::move(value)); return *this;}
    inline GetRecordsResult& AddRecords(const Record& value) { m_records.push_back(value); return *this; }
    inline GetRecordsResult& AddRecords(Record&& value) { m_records.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The next position in the shard from which to start sequentially reading data
     * records. If set to <code>null</code>, the shard has been closed and the
     * requested iterator does not return any more data. </p>
     */
    inline const Aws::String& GetNextShardIterator() const{ return m_nextShardIterator; }
    inline void SetNextShardIterator(const Aws::String& value) { m_nextShardIterator = value; }
    inline void SetNextShardIterator(Aws::String&& value) { m_nextShardIterator = std::move(value); }
    inline void SetNextShardIterator(const char* value) { m_nextShardIterator.assign(value); }
    inline GetRecordsResult& WithNextShardIterator(const Aws::String& value) { SetNextShardIterator(value); return *this;}
    inline GetRecordsResult& WithNextShardIterator(Aws::String&& value) { SetNextShardIterator(std::move(value)); return *this;}
    inline GetRecordsResult& WithNextShardIterator(const char* value) { SetNextShardIterator(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of milliseconds the <a>GetRecords</a> response is from the tip of
     * the stream, indicating how far behind current time the consumer is. A value of
     * zero indicates that record processing is caught up, and there are no new records
     * to process at this moment.</p>
     */
    inline long long GetMillisBehindLatest() const{ return m_millisBehindLatest; }
    inline void SetMillisBehindLatest(long long value) { m_millisBehindLatest = value; }
    inline GetRecordsResult& WithMillisBehindLatest(long long value) { SetMillisBehindLatest(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The list of the current shard's child shards, returned in the
     * <code>GetRecords</code> API's response only when the end of the current shard is
     * reached.</p>
     */
    inline const Aws::Vector<ChildShard>& GetChildShards() const{ return m_childShards; }
    inline void SetChildShards(const Aws::Vector<ChildShard>& value) { m_childShards = value; }
    inline void SetChildShards(Aws::Vector<ChildShard>&& value) { m_childShards = std::move(value); }
    inline GetRecordsResult& WithChildShards(const Aws::Vector<ChildShard>& value) { SetChildShards(value); return *this;}
    inline GetRecordsResult& WithChildShards(Aws::Vector<ChildShard>&& value) { SetChildShards(std::move(value)); return *this;}
    inline GetRecordsResult& AddChildShards(const ChildShard& value) { m_childShards.push_back(value); return *this; }
    inline GetRecordsResult& AddChildShards(ChildShard&& value) { m_childShards.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetRecordsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetRecordsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetRecordsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<Record> m_records;

    Aws::String m_nextShardIterator;

    long long m_millisBehindLatest;

    Aws::Vector<ChildShard> m_childShards;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
