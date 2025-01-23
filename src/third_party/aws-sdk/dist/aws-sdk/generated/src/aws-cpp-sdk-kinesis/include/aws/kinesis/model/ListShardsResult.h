/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/model/Shard.h>
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
  class ListShardsResult
  {
  public:
    AWS_KINESIS_API ListShardsResult();
    AWS_KINESIS_API ListShardsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API ListShardsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>An array of JSON objects. Each object represents one shard and specifies the
     * IDs of the shard, the shard's parent, and the shard that's adjacent to the
     * shard's parent. Each object also contains the starting and ending hash keys and
     * the starting and ending sequence numbers for the shard.</p>
     */
    inline const Aws::Vector<Shard>& GetShards() const{ return m_shards; }
    inline void SetShards(const Aws::Vector<Shard>& value) { m_shards = value; }
    inline void SetShards(Aws::Vector<Shard>&& value) { m_shards = std::move(value); }
    inline ListShardsResult& WithShards(const Aws::Vector<Shard>& value) { SetShards(value); return *this;}
    inline ListShardsResult& WithShards(Aws::Vector<Shard>&& value) { SetShards(std::move(value)); return *this;}
    inline ListShardsResult& AddShards(const Shard& value) { m_shards.push_back(value); return *this; }
    inline ListShardsResult& AddShards(Shard&& value) { m_shards.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>When the number of shards in the data stream is greater than the default
     * value for the <code>MaxResults</code> parameter, or if you explicitly specify a
     * value for <code>MaxResults</code> that is less than the number of shards in the
     * data stream, the response includes a pagination token named
     * <code>NextToken</code>. You can specify this <code>NextToken</code> value in a
     * subsequent call to <code>ListShards</code> to list the next set of shards. For
     * more information about the use of this pagination token when calling the
     * <code>ListShards</code> operation, see <a>ListShardsInput$NextToken</a>.</p>
     *  <p>Tokens expire after 300 seconds. When you obtain a value for
     * <code>NextToken</code> in the response to a call to <code>ListShards</code>, you
     * have 300 seconds to use that value. If you specify an expired token in a call to
     * <code>ListShards</code>, you get <code>ExpiredNextTokenException</code>.</p>
     * 
     */
    inline const Aws::String& GetNextToken() const{ return m_nextToken; }
    inline void SetNextToken(const Aws::String& value) { m_nextToken = value; }
    inline void SetNextToken(Aws::String&& value) { m_nextToken = std::move(value); }
    inline void SetNextToken(const char* value) { m_nextToken.assign(value); }
    inline ListShardsResult& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline ListShardsResult& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline ListShardsResult& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListShardsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListShardsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListShardsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<Shard> m_shards;

    Aws::String m_nextToken;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
