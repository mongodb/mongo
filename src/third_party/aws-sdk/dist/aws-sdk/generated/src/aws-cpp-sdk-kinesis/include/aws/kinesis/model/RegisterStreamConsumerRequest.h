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
  class RegisterStreamConsumerRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API RegisterStreamConsumerRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "RegisterStreamConsumer"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The ARN of the Kinesis data stream that you want to register the consumer
     * with. For more info, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html#arn-syntax-kinesis-streams">Amazon
     * Resource Names (ARNs) and Amazon Web Services Service Namespaces</a>.</p>
     */
    inline const Aws::String& GetStreamARN() const{ return m_streamARN; }
    inline bool StreamARNHasBeenSet() const { return m_streamARNHasBeenSet; }
    inline void SetStreamARN(const Aws::String& value) { m_streamARNHasBeenSet = true; m_streamARN = value; }
    inline void SetStreamARN(Aws::String&& value) { m_streamARNHasBeenSet = true; m_streamARN = std::move(value); }
    inline void SetStreamARN(const char* value) { m_streamARNHasBeenSet = true; m_streamARN.assign(value); }
    inline RegisterStreamConsumerRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline RegisterStreamConsumerRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline RegisterStreamConsumerRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>For a given Kinesis data stream, each consumer must have a unique name.
     * However, consumer names don't have to be unique across data streams.</p>
     */
    inline const Aws::String& GetConsumerName() const{ return m_consumerName; }
    inline bool ConsumerNameHasBeenSet() const { return m_consumerNameHasBeenSet; }
    inline void SetConsumerName(const Aws::String& value) { m_consumerNameHasBeenSet = true; m_consumerName = value; }
    inline void SetConsumerName(Aws::String&& value) { m_consumerNameHasBeenSet = true; m_consumerName = std::move(value); }
    inline void SetConsumerName(const char* value) { m_consumerNameHasBeenSet = true; m_consumerName.assign(value); }
    inline RegisterStreamConsumerRequest& WithConsumerName(const Aws::String& value) { SetConsumerName(value); return *this;}
    inline RegisterStreamConsumerRequest& WithConsumerName(Aws::String&& value) { SetConsumerName(std::move(value)); return *this;}
    inline RegisterStreamConsumerRequest& WithConsumerName(const char* value) { SetConsumerName(value); return *this;}
    ///@}
  private:

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;

    Aws::String m_consumerName;
    bool m_consumerNameHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
