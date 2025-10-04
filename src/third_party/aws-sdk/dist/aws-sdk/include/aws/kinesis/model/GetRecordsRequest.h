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
   * <p>Represents the input for <a>GetRecords</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/GetRecordsInput">AWS
   * API Reference</a></p>
   */
  class GetRecordsRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API GetRecordsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetRecords"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The position in the shard from which you want to start sequentially reading
     * data records. A shard iterator specifies this position using the sequence number
     * of a data record in the shard.</p>
     */
    inline const Aws::String& GetShardIterator() const{ return m_shardIterator; }
    inline bool ShardIteratorHasBeenSet() const { return m_shardIteratorHasBeenSet; }
    inline void SetShardIterator(const Aws::String& value) { m_shardIteratorHasBeenSet = true; m_shardIterator = value; }
    inline void SetShardIterator(Aws::String&& value) { m_shardIteratorHasBeenSet = true; m_shardIterator = std::move(value); }
    inline void SetShardIterator(const char* value) { m_shardIteratorHasBeenSet = true; m_shardIterator.assign(value); }
    inline GetRecordsRequest& WithShardIterator(const Aws::String& value) { SetShardIterator(value); return *this;}
    inline GetRecordsRequest& WithShardIterator(Aws::String&& value) { SetShardIterator(std::move(value)); return *this;}
    inline GetRecordsRequest& WithShardIterator(const char* value) { SetShardIterator(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of records to return. Specify a value of up to 10,000. If
     * you specify a value that is greater than 10,000, <a>GetRecords</a> throws
     * <code>InvalidArgumentException</code>. The default value is 10,000.</p>
     */
    inline int GetLimit() const{ return m_limit; }
    inline bool LimitHasBeenSet() const { return m_limitHasBeenSet; }
    inline void SetLimit(int value) { m_limitHasBeenSet = true; m_limit = value; }
    inline GetRecordsRequest& WithLimit(int value) { SetLimit(value); return *this;}
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
    inline GetRecordsRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline GetRecordsRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline GetRecordsRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}
  private:

    Aws::String m_shardIterator;
    bool m_shardIteratorHasBeenSet = false;

    int m_limit;
    bool m_limitHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
