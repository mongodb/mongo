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
   * <p>Represents the input for <a>DeleteStream</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/DeleteStreamInput">AWS
   * API Reference</a></p>
   */
  class DeleteStreamRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API DeleteStreamRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DeleteStream"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the stream to delete.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline bool StreamNameHasBeenSet() const { return m_streamNameHasBeenSet; }
    inline void SetStreamName(const Aws::String& value) { m_streamNameHasBeenSet = true; m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamNameHasBeenSet = true; m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamNameHasBeenSet = true; m_streamName.assign(value); }
    inline DeleteStreamRequest& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline DeleteStreamRequest& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline DeleteStreamRequest& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If this parameter is unset (<code>null</code>) or if you set it to
     * <code>false</code>, and the stream has registered consumers, the call to
     * <code>DeleteStream</code> fails with a <code>ResourceInUseException</code>. </p>
     */
    inline bool GetEnforceConsumerDeletion() const{ return m_enforceConsumerDeletion; }
    inline bool EnforceConsumerDeletionHasBeenSet() const { return m_enforceConsumerDeletionHasBeenSet; }
    inline void SetEnforceConsumerDeletion(bool value) { m_enforceConsumerDeletionHasBeenSet = true; m_enforceConsumerDeletion = value; }
    inline DeleteStreamRequest& WithEnforceConsumerDeletion(bool value) { SetEnforceConsumerDeletion(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the stream.</p>
     */
    inline const Aws::String& GetStreamARN() const{ return m_streamARN; }
    inline bool StreamARNHasBeenSet() const { return m_streamARNHasBeenSet; }
    inline void SetStreamARN(const Aws::String& value) { m_streamARNHasBeenSet = true; m_streamARN = value; }
    inline void SetStreamARN(Aws::String&& value) { m_streamARNHasBeenSet = true; m_streamARN = std::move(value); }
    inline void SetStreamARN(const char* value) { m_streamARNHasBeenSet = true; m_streamARN.assign(value); }
    inline DeleteStreamRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline DeleteStreamRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline DeleteStreamRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;
    bool m_streamNameHasBeenSet = false;

    bool m_enforceConsumerDeletion;
    bool m_enforceConsumerDeletionHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
