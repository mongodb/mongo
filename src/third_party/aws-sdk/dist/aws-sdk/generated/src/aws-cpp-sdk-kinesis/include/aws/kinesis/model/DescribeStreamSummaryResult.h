/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/model/StreamDescriptionSummary.h>
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
  class DescribeStreamSummaryResult
  {
  public:
    AWS_KINESIS_API DescribeStreamSummaryResult();
    AWS_KINESIS_API DescribeStreamSummaryResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API DescribeStreamSummaryResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A <a>StreamDescriptionSummary</a> containing information about the
     * stream.</p>
     */
    inline const StreamDescriptionSummary& GetStreamDescriptionSummary() const{ return m_streamDescriptionSummary; }
    inline void SetStreamDescriptionSummary(const StreamDescriptionSummary& value) { m_streamDescriptionSummary = value; }
    inline void SetStreamDescriptionSummary(StreamDescriptionSummary&& value) { m_streamDescriptionSummary = std::move(value); }
    inline DescribeStreamSummaryResult& WithStreamDescriptionSummary(const StreamDescriptionSummary& value) { SetStreamDescriptionSummary(value); return *this;}
    inline DescribeStreamSummaryResult& WithStreamDescriptionSummary(StreamDescriptionSummary&& value) { SetStreamDescriptionSummary(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline DescribeStreamSummaryResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline DescribeStreamSummaryResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline DescribeStreamSummaryResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    StreamDescriptionSummary m_streamDescriptionSummary;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
