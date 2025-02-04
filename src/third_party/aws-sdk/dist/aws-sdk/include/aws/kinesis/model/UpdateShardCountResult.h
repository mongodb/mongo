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
  class UpdateShardCountResult
  {
  public:
    AWS_KINESIS_API UpdateShardCountResult();
    AWS_KINESIS_API UpdateShardCountResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API UpdateShardCountResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The name of the stream.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline void SetStreamName(const Aws::String& value) { m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamName.assign(value); }
    inline UpdateShardCountResult& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline UpdateShardCountResult& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline UpdateShardCountResult& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The current number of shards.</p>
     */
    inline int GetCurrentShardCount() const{ return m_currentShardCount; }
    inline void SetCurrentShardCount(int value) { m_currentShardCount = value; }
    inline UpdateShardCountResult& WithCurrentShardCount(int value) { SetCurrentShardCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The updated number of shards.</p>
     */
    inline int GetTargetShardCount() const{ return m_targetShardCount; }
    inline void SetTargetShardCount(int value) { m_targetShardCount = value; }
    inline UpdateShardCountResult& WithTargetShardCount(int value) { SetTargetShardCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the stream.</p>
     */
    inline const Aws::String& GetStreamARN() const{ return m_streamARN; }
    inline void SetStreamARN(const Aws::String& value) { m_streamARN = value; }
    inline void SetStreamARN(Aws::String&& value) { m_streamARN = std::move(value); }
    inline void SetStreamARN(const char* value) { m_streamARN.assign(value); }
    inline UpdateShardCountResult& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline UpdateShardCountResult& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline UpdateShardCountResult& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline UpdateShardCountResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline UpdateShardCountResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline UpdateShardCountResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;

    int m_currentShardCount;

    int m_targetShardCount;

    Aws::String m_streamARN;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
