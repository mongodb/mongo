/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/kinesis/model/HashKeyRange.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Kinesis
{
namespace Model
{

  /**
   * <p>Output parameter of the GetRecords API. The existing child shard of the
   * current shard.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/ChildShard">AWS
   * API Reference</a></p>
   */
  class ChildShard
  {
  public:
    AWS_KINESIS_API ChildShard();
    AWS_KINESIS_API ChildShard(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API ChildShard& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The shard ID of the existing child shard of the current shard.</p>
     */
    inline const Aws::String& GetShardId() const{ return m_shardId; }
    inline bool ShardIdHasBeenSet() const { return m_shardIdHasBeenSet; }
    inline void SetShardId(const Aws::String& value) { m_shardIdHasBeenSet = true; m_shardId = value; }
    inline void SetShardId(Aws::String&& value) { m_shardIdHasBeenSet = true; m_shardId = std::move(value); }
    inline void SetShardId(const char* value) { m_shardIdHasBeenSet = true; m_shardId.assign(value); }
    inline ChildShard& WithShardId(const Aws::String& value) { SetShardId(value); return *this;}
    inline ChildShard& WithShardId(Aws::String&& value) { SetShardId(std::move(value)); return *this;}
    inline ChildShard& WithShardId(const char* value) { SetShardId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The current shard that is the parent of the existing child shard.</p>
     */
    inline const Aws::Vector<Aws::String>& GetParentShards() const{ return m_parentShards; }
    inline bool ParentShardsHasBeenSet() const { return m_parentShardsHasBeenSet; }
    inline void SetParentShards(const Aws::Vector<Aws::String>& value) { m_parentShardsHasBeenSet = true; m_parentShards = value; }
    inline void SetParentShards(Aws::Vector<Aws::String>&& value) { m_parentShardsHasBeenSet = true; m_parentShards = std::move(value); }
    inline ChildShard& WithParentShards(const Aws::Vector<Aws::String>& value) { SetParentShards(value); return *this;}
    inline ChildShard& WithParentShards(Aws::Vector<Aws::String>&& value) { SetParentShards(std::move(value)); return *this;}
    inline ChildShard& AddParentShards(const Aws::String& value) { m_parentShardsHasBeenSet = true; m_parentShards.push_back(value); return *this; }
    inline ChildShard& AddParentShards(Aws::String&& value) { m_parentShardsHasBeenSet = true; m_parentShards.push_back(std::move(value)); return *this; }
    inline ChildShard& AddParentShards(const char* value) { m_parentShardsHasBeenSet = true; m_parentShards.push_back(value); return *this; }
    ///@}

    ///@{
    
    inline const HashKeyRange& GetHashKeyRange() const{ return m_hashKeyRange; }
    inline bool HashKeyRangeHasBeenSet() const { return m_hashKeyRangeHasBeenSet; }
    inline void SetHashKeyRange(const HashKeyRange& value) { m_hashKeyRangeHasBeenSet = true; m_hashKeyRange = value; }
    inline void SetHashKeyRange(HashKeyRange&& value) { m_hashKeyRangeHasBeenSet = true; m_hashKeyRange = std::move(value); }
    inline ChildShard& WithHashKeyRange(const HashKeyRange& value) { SetHashKeyRange(value); return *this;}
    inline ChildShard& WithHashKeyRange(HashKeyRange&& value) { SetHashKeyRange(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_shardId;
    bool m_shardIdHasBeenSet = false;

    Aws::Vector<Aws::String> m_parentShards;
    bool m_parentShardsHasBeenSet = false;

    HashKeyRange m_hashKeyRange;
    bool m_hashKeyRangeHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
