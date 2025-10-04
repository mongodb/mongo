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
   * <p>Represents the input for <code>SplitShard</code>.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/SplitShardInput">AWS
   * API Reference</a></p>
   */
  class SplitShardRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API SplitShardRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "SplitShard"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the stream for the shard split.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline bool StreamNameHasBeenSet() const { return m_streamNameHasBeenSet; }
    inline void SetStreamName(const Aws::String& value) { m_streamNameHasBeenSet = true; m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamNameHasBeenSet = true; m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamNameHasBeenSet = true; m_streamName.assign(value); }
    inline SplitShardRequest& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline SplitShardRequest& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline SplitShardRequest& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The shard ID of the shard to split.</p>
     */
    inline const Aws::String& GetShardToSplit() const{ return m_shardToSplit; }
    inline bool ShardToSplitHasBeenSet() const { return m_shardToSplitHasBeenSet; }
    inline void SetShardToSplit(const Aws::String& value) { m_shardToSplitHasBeenSet = true; m_shardToSplit = value; }
    inline void SetShardToSplit(Aws::String&& value) { m_shardToSplitHasBeenSet = true; m_shardToSplit = std::move(value); }
    inline void SetShardToSplit(const char* value) { m_shardToSplitHasBeenSet = true; m_shardToSplit.assign(value); }
    inline SplitShardRequest& WithShardToSplit(const Aws::String& value) { SetShardToSplit(value); return *this;}
    inline SplitShardRequest& WithShardToSplit(Aws::String&& value) { SetShardToSplit(std::move(value)); return *this;}
    inline SplitShardRequest& WithShardToSplit(const char* value) { SetShardToSplit(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A hash key value for the starting hash key of one of the child shards created
     * by the split. The hash key range for a given shard constitutes a set of ordered
     * contiguous positive integers. The value for <code>NewStartingHashKey</code> must
     * be in the range of hash keys being mapped into the shard. The
     * <code>NewStartingHashKey</code> hash key value and all higher hash key values in
     * hash key range are distributed to one of the child shards. All the lower hash
     * key values in the range are distributed to the other child shard.</p>
     */
    inline const Aws::String& GetNewStartingHashKey() const{ return m_newStartingHashKey; }
    inline bool NewStartingHashKeyHasBeenSet() const { return m_newStartingHashKeyHasBeenSet; }
    inline void SetNewStartingHashKey(const Aws::String& value) { m_newStartingHashKeyHasBeenSet = true; m_newStartingHashKey = value; }
    inline void SetNewStartingHashKey(Aws::String&& value) { m_newStartingHashKeyHasBeenSet = true; m_newStartingHashKey = std::move(value); }
    inline void SetNewStartingHashKey(const char* value) { m_newStartingHashKeyHasBeenSet = true; m_newStartingHashKey.assign(value); }
    inline SplitShardRequest& WithNewStartingHashKey(const Aws::String& value) { SetNewStartingHashKey(value); return *this;}
    inline SplitShardRequest& WithNewStartingHashKey(Aws::String&& value) { SetNewStartingHashKey(std::move(value)); return *this;}
    inline SplitShardRequest& WithNewStartingHashKey(const char* value) { SetNewStartingHashKey(value); return *this;}
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
    inline SplitShardRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline SplitShardRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline SplitShardRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;
    bool m_streamNameHasBeenSet = false;

    Aws::String m_shardToSplit;
    bool m_shardToSplitHasBeenSet = false;

    Aws::String m_newStartingHashKey;
    bool m_newStartingHashKeyHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
