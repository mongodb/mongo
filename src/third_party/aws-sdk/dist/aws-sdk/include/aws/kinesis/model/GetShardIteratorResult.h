/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Represents the output for <code>GetShardIterator</code>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/GetShardIteratorOutput">AWS
   * API Reference</a></p>
   */
  class GetShardIteratorResult
  {
  public:
    AWS_KINESIS_API GetShardIteratorResult();
    AWS_KINESIS_API GetShardIteratorResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API GetShardIteratorResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The position in the shard from which to start reading data records
     * sequentially. A shard iterator specifies this position using the sequence number
     * of a data record in a shard.</p>
     */
    inline const Aws::String& GetShardIterator() const{ return m_shardIterator; }
    inline void SetShardIterator(const Aws::String& value) { m_shardIterator = value; }
    inline void SetShardIterator(Aws::String&& value) { m_shardIterator = std::move(value); }
    inline void SetShardIterator(const char* value) { m_shardIterator.assign(value); }
    inline GetShardIteratorResult& WithShardIterator(const Aws::String& value) { SetShardIterator(value); return *this;}
    inline GetShardIteratorResult& WithShardIterator(Aws::String&& value) { SetShardIterator(std::move(value)); return *this;}
    inline GetShardIteratorResult& WithShardIterator(const char* value) { SetShardIterator(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetShardIteratorResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetShardIteratorResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetShardIteratorResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_shardIterator;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
