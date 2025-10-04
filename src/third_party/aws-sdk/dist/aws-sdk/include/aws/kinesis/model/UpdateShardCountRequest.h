/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/KinesisRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/model/ScalingType.h>
#include <utility>

namespace Aws
{
namespace Kinesis
{
namespace Model
{

  /**
   */
  class UpdateShardCountRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API UpdateShardCountRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateShardCount"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the stream.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline bool StreamNameHasBeenSet() const { return m_streamNameHasBeenSet; }
    inline void SetStreamName(const Aws::String& value) { m_streamNameHasBeenSet = true; m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamNameHasBeenSet = true; m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamNameHasBeenSet = true; m_streamName.assign(value); }
    inline UpdateShardCountRequest& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline UpdateShardCountRequest& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline UpdateShardCountRequest& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The new number of shards. This value has the following default limits. By
     * default, you cannot do the following: </p> <ul> <li> <p>Set this value to more
     * than double your current shard count for a stream.</p> </li> <li> <p>Set this
     * value below half your current shard count for a stream.</p> </li> <li> <p>Set
     * this value to more than 10000 shards in a stream (the default limit for shard
     * count per stream is 10000 per account per region), unless you request a limit
     * increase.</p> </li> <li> <p>Scale a stream with more than 10000 shards down
     * unless you set this value to less than 10000 shards.</p> </li> </ul>
     */
    inline int GetTargetShardCount() const{ return m_targetShardCount; }
    inline bool TargetShardCountHasBeenSet() const { return m_targetShardCountHasBeenSet; }
    inline void SetTargetShardCount(int value) { m_targetShardCountHasBeenSet = true; m_targetShardCount = value; }
    inline UpdateShardCountRequest& WithTargetShardCount(int value) { SetTargetShardCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The scaling type. Uniform scaling creates shards of equal size.</p>
     */
    inline const ScalingType& GetScalingType() const{ return m_scalingType; }
    inline bool ScalingTypeHasBeenSet() const { return m_scalingTypeHasBeenSet; }
    inline void SetScalingType(const ScalingType& value) { m_scalingTypeHasBeenSet = true; m_scalingType = value; }
    inline void SetScalingType(ScalingType&& value) { m_scalingTypeHasBeenSet = true; m_scalingType = std::move(value); }
    inline UpdateShardCountRequest& WithScalingType(const ScalingType& value) { SetScalingType(value); return *this;}
    inline UpdateShardCountRequest& WithScalingType(ScalingType&& value) { SetScalingType(std::move(value)); return *this;}
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
    inline UpdateShardCountRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline UpdateShardCountRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline UpdateShardCountRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;
    bool m_streamNameHasBeenSet = false;

    int m_targetShardCount;
    bool m_targetShardCountHasBeenSet = false;

    ScalingType m_scalingType;
    bool m_scalingTypeHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
