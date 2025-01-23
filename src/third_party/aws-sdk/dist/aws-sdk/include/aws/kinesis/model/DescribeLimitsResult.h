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
  class DescribeLimitsResult
  {
  public:
    AWS_KINESIS_API DescribeLimitsResult();
    AWS_KINESIS_API DescribeLimitsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API DescribeLimitsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The maximum number of shards.</p>
     */
    inline int GetShardLimit() const{ return m_shardLimit; }
    inline void SetShardLimit(int value) { m_shardLimit = value; }
    inline DescribeLimitsResult& WithShardLimit(int value) { SetShardLimit(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of open shards.</p>
     */
    inline int GetOpenShardCount() const{ return m_openShardCount; }
    inline void SetOpenShardCount(int value) { m_openShardCount = value; }
    inline DescribeLimitsResult& WithOpenShardCount(int value) { SetOpenShardCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> Indicates the number of data streams with the on-demand capacity mode.</p>
     */
    inline int GetOnDemandStreamCount() const{ return m_onDemandStreamCount; }
    inline void SetOnDemandStreamCount(int value) { m_onDemandStreamCount = value; }
    inline DescribeLimitsResult& WithOnDemandStreamCount(int value) { SetOnDemandStreamCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The maximum number of data streams with the on-demand capacity mode. </p>
     */
    inline int GetOnDemandStreamCountLimit() const{ return m_onDemandStreamCountLimit; }
    inline void SetOnDemandStreamCountLimit(int value) { m_onDemandStreamCountLimit = value; }
    inline DescribeLimitsResult& WithOnDemandStreamCountLimit(int value) { SetOnDemandStreamCountLimit(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline DescribeLimitsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline DescribeLimitsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline DescribeLimitsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    int m_shardLimit;

    int m_openShardCount;

    int m_onDemandStreamCount;

    int m_onDemandStreamCountLimit;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
