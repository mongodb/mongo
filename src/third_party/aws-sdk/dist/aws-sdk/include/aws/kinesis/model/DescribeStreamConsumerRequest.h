/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/KinesisRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Kinesis
{
namespace Model
{

  /**
   */
  class DescribeStreamConsumerRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API DescribeStreamConsumerRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DescribeStreamConsumer"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The ARN of the Kinesis data stream that the consumer is registered with. For
     * more information, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html#arn-syntax-kinesis-streams">Amazon
     * Resource Names (ARNs) and Amazon Web Services Service Namespaces</a>.</p>
     */
    inline const Aws::String& GetStreamARN() const{ return m_streamARN; }
    inline bool StreamARNHasBeenSet() const { return m_streamARNHasBeenSet; }
    inline void SetStreamARN(const Aws::String& value) { m_streamARNHasBeenSet = true; m_streamARN = value; }
    inline void SetStreamARN(Aws::String&& value) { m_streamARNHasBeenSet = true; m_streamARN = std::move(value); }
    inline void SetStreamARN(const char* value) { m_streamARNHasBeenSet = true; m_streamARN.assign(value); }
    inline DescribeStreamConsumerRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline DescribeStreamConsumerRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline DescribeStreamConsumerRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name that you gave to the consumer.</p>
     */
    inline const Aws::String& GetConsumerName() const{ return m_consumerName; }
    inline bool ConsumerNameHasBeenSet() const { return m_consumerNameHasBeenSet; }
    inline void SetConsumerName(const Aws::String& value) { m_consumerNameHasBeenSet = true; m_consumerName = value; }
    inline void SetConsumerName(Aws::String&& value) { m_consumerNameHasBeenSet = true; m_consumerName = std::move(value); }
    inline void SetConsumerName(const char* value) { m_consumerNameHasBeenSet = true; m_consumerName.assign(value); }
    inline DescribeStreamConsumerRequest& WithConsumerName(const Aws::String& value) { SetConsumerName(value); return *this;}
    inline DescribeStreamConsumerRequest& WithConsumerName(Aws::String&& value) { SetConsumerName(std::move(value)); return *this;}
    inline DescribeStreamConsumerRequest& WithConsumerName(const char* value) { SetConsumerName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN returned by Kinesis Data Streams when you registered the
     * consumer.</p>
     */
    inline const Aws::String& GetConsumerARN() const{ return m_consumerARN; }
    inline bool ConsumerARNHasBeenSet() const { return m_consumerARNHasBeenSet; }
    inline void SetConsumerARN(const Aws::String& value) { m_consumerARNHasBeenSet = true; m_consumerARN = value; }
    inline void SetConsumerARN(Aws::String&& value) { m_consumerARNHasBeenSet = true; m_consumerARN = std::move(value); }
    inline void SetConsumerARN(const char* value) { m_consumerARNHasBeenSet = true; m_consumerARN.assign(value); }
    inline DescribeStreamConsumerRequest& WithConsumerARN(const Aws::String& value) { SetConsumerARN(value); return *this;}
    inline DescribeStreamConsumerRequest& WithConsumerARN(Aws::String&& value) { SetConsumerARN(std::move(value)); return *this;}
    inline DescribeStreamConsumerRequest& WithConsumerARN(const char* value) { SetConsumerARN(value); return *this;}
    ///@}
  private:

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;

    Aws::String m_consumerName;
    bool m_consumerNameHasBeenSet = false;

    Aws::String m_consumerARN;
    bool m_consumerARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
