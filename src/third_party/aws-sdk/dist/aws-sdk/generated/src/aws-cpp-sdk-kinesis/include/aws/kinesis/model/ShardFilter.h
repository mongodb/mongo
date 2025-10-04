/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/model/ShardFilterType.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
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
   * <p>The request parameter used to filter out the response of the
   * <code>ListShards</code> API.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/ShardFilter">AWS
   * API Reference</a></p>
   */
  class ShardFilter
  {
  public:
    AWS_KINESIS_API ShardFilter();
    AWS_KINESIS_API ShardFilter(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API ShardFilter& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The shard type specified in the <code>ShardFilter</code> parameter. This is a
     * required property of the <code>ShardFilter</code> parameter.</p> <p>You can
     * specify the following valid values: </p> <ul> <li> <p>
     * <code>AFTER_SHARD_ID</code> - the response includes all the shards, starting
     * with the shard whose ID immediately follows the <code>ShardId</code> that you
     * provided. </p> </li> <li> <p> <code>AT_TRIM_HORIZON</code> - the response
     * includes all the shards that were open at <code>TRIM_HORIZON</code>.</p> </li>
     * <li> <p> <code>FROM_TRIM_HORIZON</code> - (default), the response includes all
     * the shards within the retention period of the data stream (trim to tip).</p>
     * </li> <li> <p> <code>AT_LATEST</code> - the response includes only the currently
     * open shards of the data stream.</p> </li> <li> <p> <code>AT_TIMESTAMP</code> -
     * the response includes all shards whose start timestamp is less than or equal to
     * the given timestamp and end timestamp is greater than or equal to the given
     * timestamp or still open. </p> </li> <li> <p> <code>FROM_TIMESTAMP</code> - the
     * response incldues all closed shards whose end timestamp is greater than or equal
     * to the given timestamp and also all open shards. Corrected to
     * <code>TRIM_HORIZON</code> of the data stream if <code>FROM_TIMESTAMP</code> is
     * less than the <code>TRIM_HORIZON</code> value.</p> </li> </ul>
     */
    inline const ShardFilterType& GetType() const{ return m_type; }
    inline bool TypeHasBeenSet() const { return m_typeHasBeenSet; }
    inline void SetType(const ShardFilterType& value) { m_typeHasBeenSet = true; m_type = value; }
    inline void SetType(ShardFilterType&& value) { m_typeHasBeenSet = true; m_type = std::move(value); }
    inline ShardFilter& WithType(const ShardFilterType& value) { SetType(value); return *this;}
    inline ShardFilter& WithType(ShardFilterType&& value) { SetType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The exclusive start <code>shardID</code> speified in the
     * <code>ShardFilter</code> parameter. This property can only be used if the
     * <code>AFTER_SHARD_ID</code> shard type is specified.</p>
     */
    inline const Aws::String& GetShardId() const{ return m_shardId; }
    inline bool ShardIdHasBeenSet() const { return m_shardIdHasBeenSet; }
    inline void SetShardId(const Aws::String& value) { m_shardIdHasBeenSet = true; m_shardId = value; }
    inline void SetShardId(Aws::String&& value) { m_shardIdHasBeenSet = true; m_shardId = std::move(value); }
    inline void SetShardId(const char* value) { m_shardIdHasBeenSet = true; m_shardId.assign(value); }
    inline ShardFilter& WithShardId(const Aws::String& value) { SetShardId(value); return *this;}
    inline ShardFilter& WithShardId(Aws::String&& value) { SetShardId(std::move(value)); return *this;}
    inline ShardFilter& WithShardId(const char* value) { SetShardId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The timestamps specified in the <code>ShardFilter</code> parameter. A
     * timestamp is a Unix epoch date with precision in milliseconds. For example,
     * 2016-04-04T19:58:46.480-00:00 or 1459799926.480. This property can only be used
     * if <code>FROM_TIMESTAMP</code> or <code>AT_TIMESTAMP</code> shard types are
     * specified.</p>
     */
    inline const Aws::Utils::DateTime& GetTimestamp() const{ return m_timestamp; }
    inline bool TimestampHasBeenSet() const { return m_timestampHasBeenSet; }
    inline void SetTimestamp(const Aws::Utils::DateTime& value) { m_timestampHasBeenSet = true; m_timestamp = value; }
    inline void SetTimestamp(Aws::Utils::DateTime&& value) { m_timestampHasBeenSet = true; m_timestamp = std::move(value); }
    inline ShardFilter& WithTimestamp(const Aws::Utils::DateTime& value) { SetTimestamp(value); return *this;}
    inline ShardFilter& WithTimestamp(Aws::Utils::DateTime&& value) { SetTimestamp(std::move(value)); return *this;}
    ///@}
  private:

    ShardFilterType m_type;
    bool m_typeHasBeenSet = false;

    Aws::String m_shardId;
    bool m_shardIdHasBeenSet = false;

    Aws::Utils::DateTime m_timestamp;
    bool m_timestampHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
