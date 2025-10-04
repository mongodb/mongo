/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/model/ConsumerDescription.h>
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
  class DescribeStreamConsumerResult
  {
  public:
    AWS_KINESIS_API DescribeStreamConsumerResult();
    AWS_KINESIS_API DescribeStreamConsumerResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API DescribeStreamConsumerResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>An object that represents the details of the consumer.</p>
     */
    inline const ConsumerDescription& GetConsumerDescription() const{ return m_consumerDescription; }
    inline void SetConsumerDescription(const ConsumerDescription& value) { m_consumerDescription = value; }
    inline void SetConsumerDescription(ConsumerDescription&& value) { m_consumerDescription = std::move(value); }
    inline DescribeStreamConsumerResult& WithConsumerDescription(const ConsumerDescription& value) { SetConsumerDescription(value); return *this;}
    inline DescribeStreamConsumerResult& WithConsumerDescription(ConsumerDescription&& value) { SetConsumerDescription(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline DescribeStreamConsumerResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline DescribeStreamConsumerResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline DescribeStreamConsumerResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    ConsumerDescription m_consumerDescription;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
