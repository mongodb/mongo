/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/model/StreamDescription.h>
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
   * <p>Represents the output for <code>DescribeStream</code>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/DescribeStreamOutput">AWS
   * API Reference</a></p>
   */
  class DescribeStreamResult
  {
  public:
    AWS_KINESIS_API DescribeStreamResult();
    AWS_KINESIS_API DescribeStreamResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API DescribeStreamResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The current status of the stream, the stream Amazon Resource Name (ARN), an
     * array of shard objects that comprise the stream, and whether there are more
     * shards available.</p>
     */
    inline const StreamDescription& GetStreamDescription() const{ return m_streamDescription; }
    inline void SetStreamDescription(const StreamDescription& value) { m_streamDescription = value; }
    inline void SetStreamDescription(StreamDescription&& value) { m_streamDescription = std::move(value); }
    inline DescribeStreamResult& WithStreamDescription(const StreamDescription& value) { SetStreamDescription(value); return *this;}
    inline DescribeStreamResult& WithStreamDescription(StreamDescription&& value) { SetStreamDescription(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline DescribeStreamResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline DescribeStreamResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline DescribeStreamResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    StreamDescription m_streamDescription;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
