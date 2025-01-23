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
   * <p>Represents the input for <code>MergeShards</code>.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/MergeShardsInput">AWS
   * API Reference</a></p>
   */
  class MergeShardsRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API MergeShardsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "MergeShards"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the stream for the merge.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline bool StreamNameHasBeenSet() const { return m_streamNameHasBeenSet; }
    inline void SetStreamName(const Aws::String& value) { m_streamNameHasBeenSet = true; m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamNameHasBeenSet = true; m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamNameHasBeenSet = true; m_streamName.assign(value); }
    inline MergeShardsRequest& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline MergeShardsRequest& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline MergeShardsRequest& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The shard ID of the shard to combine with the adjacent shard for the
     * merge.</p>
     */
    inline const Aws::String& GetShardToMerge() const{ return m_shardToMerge; }
    inline bool ShardToMergeHasBeenSet() const { return m_shardToMergeHasBeenSet; }
    inline void SetShardToMerge(const Aws::String& value) { m_shardToMergeHasBeenSet = true; m_shardToMerge = value; }
    inline void SetShardToMerge(Aws::String&& value) { m_shardToMergeHasBeenSet = true; m_shardToMerge = std::move(value); }
    inline void SetShardToMerge(const char* value) { m_shardToMergeHasBeenSet = true; m_shardToMerge.assign(value); }
    inline MergeShardsRequest& WithShardToMerge(const Aws::String& value) { SetShardToMerge(value); return *this;}
    inline MergeShardsRequest& WithShardToMerge(Aws::String&& value) { SetShardToMerge(std::move(value)); return *this;}
    inline MergeShardsRequest& WithShardToMerge(const char* value) { SetShardToMerge(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The shard ID of the adjacent shard for the merge.</p>
     */
    inline const Aws::String& GetAdjacentShardToMerge() const{ return m_adjacentShardToMerge; }
    inline bool AdjacentShardToMergeHasBeenSet() const { return m_adjacentShardToMergeHasBeenSet; }
    inline void SetAdjacentShardToMerge(const Aws::String& value) { m_adjacentShardToMergeHasBeenSet = true; m_adjacentShardToMerge = value; }
    inline void SetAdjacentShardToMerge(Aws::String&& value) { m_adjacentShardToMergeHasBeenSet = true; m_adjacentShardToMerge = std::move(value); }
    inline void SetAdjacentShardToMerge(const char* value) { m_adjacentShardToMergeHasBeenSet = true; m_adjacentShardToMerge.assign(value); }
    inline MergeShardsRequest& WithAdjacentShardToMerge(const Aws::String& value) { SetAdjacentShardToMerge(value); return *this;}
    inline MergeShardsRequest& WithAdjacentShardToMerge(Aws::String&& value) { SetAdjacentShardToMerge(std::move(value)); return *this;}
    inline MergeShardsRequest& WithAdjacentShardToMerge(const char* value) { SetAdjacentShardToMerge(value); return *this;}
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
    inline MergeShardsRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline MergeShardsRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline MergeShardsRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;
    bool m_streamNameHasBeenSet = false;

    Aws::String m_shardToMerge;
    bool m_shardToMergeHasBeenSet = false;

    Aws::String m_adjacentShardToMerge;
    bool m_adjacentShardToMergeHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
