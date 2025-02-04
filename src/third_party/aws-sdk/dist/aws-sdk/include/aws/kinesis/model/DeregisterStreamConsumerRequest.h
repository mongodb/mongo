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
  class DeregisterStreamConsumerRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API DeregisterStreamConsumerRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DeregisterStreamConsumer"; }

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
    inline DeregisterStreamConsumerRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline DeregisterStreamConsumerRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline DeregisterStreamConsumerRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
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
    inline DeregisterStreamConsumerRequest& WithConsumerName(const Aws::String& value) { SetConsumerName(value); return *this;}
    inline DeregisterStreamConsumerRequest& WithConsumerName(Aws::String&& value) { SetConsumerName(std::move(value)); return *this;}
    inline DeregisterStreamConsumerRequest& WithConsumerName(const char* value) { SetConsumerName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN returned by Kinesis Data Streams when you registered the consumer. If
     * you don't know the ARN of the consumer that you want to deregister, you can use
     * the ListStreamConsumers operation to get a list of the descriptions of all the
     * consumers that are currently registered with a given data stream. The
     * description of a consumer contains its ARN.</p>
     */
    inline const Aws::String& GetConsumerARN() const{ return m_consumerARN; }
    inline bool ConsumerARNHasBeenSet() const { return m_consumerARNHasBeenSet; }
    inline void SetConsumerARN(const Aws::String& value) { m_consumerARNHasBeenSet = true; m_consumerARN = value; }
    inline void SetConsumerARN(Aws::String&& value) { m_consumerARNHasBeenSet = true; m_consumerARN = std::move(value); }
    inline void SetConsumerARN(const char* value) { m_consumerARNHasBeenSet = true; m_consumerARN.assign(value); }
    inline DeregisterStreamConsumerRequest& WithConsumerARN(const Aws::String& value) { SetConsumerARN(value); return *this;}
    inline DeregisterStreamConsumerRequest& WithConsumerARN(Aws::String&& value) { SetConsumerARN(std::move(value)); return *this;}
    inline DeregisterStreamConsumerRequest& WithConsumerARN(const char* value) { SetConsumerARN(value); return *this;}
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
