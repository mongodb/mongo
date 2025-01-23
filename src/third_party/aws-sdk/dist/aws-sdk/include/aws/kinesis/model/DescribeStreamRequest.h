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
   * <p>Represents the input for <code>DescribeStream</code>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/DescribeStreamInput">AWS
   * API Reference</a></p>
   */
  class DescribeStreamRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API DescribeStreamRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DescribeStream"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the stream to describe.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline bool StreamNameHasBeenSet() const { return m_streamNameHasBeenSet; }
    inline void SetStreamName(const Aws::String& value) { m_streamNameHasBeenSet = true; m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamNameHasBeenSet = true; m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamNameHasBeenSet = true; m_streamName.assign(value); }
    inline DescribeStreamRequest& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline DescribeStreamRequest& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline DescribeStreamRequest& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of shards to return in a single call. The default value is
     * 100. If you specify a value greater than 100, at most 100 results are
     * returned.</p>
     */
    inline int GetLimit() const{ return m_limit; }
    inline bool LimitHasBeenSet() const { return m_limitHasBeenSet; }
    inline void SetLimit(int value) { m_limitHasBeenSet = true; m_limit = value; }
    inline DescribeStreamRequest& WithLimit(int value) { SetLimit(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The shard ID of the shard to start with.</p> <p>Specify this parameter to
     * indicate that you want to describe the stream starting with the shard whose ID
     * immediately follows <code>ExclusiveStartShardId</code>.</p> <p>If you don't
     * specify this parameter, the default behavior for <code>DescribeStream</code> is
     * to describe the stream starting with the first shard in the stream.</p>
     */
    inline const Aws::String& GetExclusiveStartShardId() const{ return m_exclusiveStartShardId; }
    inline bool ExclusiveStartShardIdHasBeenSet() const { return m_exclusiveStartShardIdHasBeenSet; }
    inline void SetExclusiveStartShardId(const Aws::String& value) { m_exclusiveStartShardIdHasBeenSet = true; m_exclusiveStartShardId = value; }
    inline void SetExclusiveStartShardId(Aws::String&& value) { m_exclusiveStartShardIdHasBeenSet = true; m_exclusiveStartShardId = std::move(value); }
    inline void SetExclusiveStartShardId(const char* value) { m_exclusiveStartShardIdHasBeenSet = true; m_exclusiveStartShardId.assign(value); }
    inline DescribeStreamRequest& WithExclusiveStartShardId(const Aws::String& value) { SetExclusiveStartShardId(value); return *this;}
    inline DescribeStreamRequest& WithExclusiveStartShardId(Aws::String&& value) { SetExclusiveStartShardId(std::move(value)); return *this;}
    inline DescribeStreamRequest& WithExclusiveStartShardId(const char* value) { SetExclusiveStartShardId(value); return *this;}
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
    inline DescribeStreamRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline DescribeStreamRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline DescribeStreamRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;
    bool m_streamNameHasBeenSet = false;

    int m_limit;
    bool m_limitHasBeenSet = false;

    Aws::String m_exclusiveStartShardId;
    bool m_exclusiveStartShardIdHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
