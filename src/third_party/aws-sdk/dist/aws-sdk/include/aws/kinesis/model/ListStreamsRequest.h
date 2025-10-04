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
   * <p>Represents the input for <code>ListStreams</code>.</p><p><h3>See Also:</h3>  
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/ListStreamsInput">AWS
   * API Reference</a></p>
   */
  class ListStreamsRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API ListStreamsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListStreams"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>The maximum number of streams to list. The default value is 100. If you
     * specify a value greater than 100, at most 100 results are returned.</p>
     */
    inline int GetLimit() const{ return m_limit; }
    inline bool LimitHasBeenSet() const { return m_limitHasBeenSet; }
    inline void SetLimit(int value) { m_limitHasBeenSet = true; m_limit = value; }
    inline ListStreamsRequest& WithLimit(int value) { SetLimit(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the stream to start the list with.</p>
     */
    inline const Aws::String& GetExclusiveStartStreamName() const{ return m_exclusiveStartStreamName; }
    inline bool ExclusiveStartStreamNameHasBeenSet() const { return m_exclusiveStartStreamNameHasBeenSet; }
    inline void SetExclusiveStartStreamName(const Aws::String& value) { m_exclusiveStartStreamNameHasBeenSet = true; m_exclusiveStartStreamName = value; }
    inline void SetExclusiveStartStreamName(Aws::String&& value) { m_exclusiveStartStreamNameHasBeenSet = true; m_exclusiveStartStreamName = std::move(value); }
    inline void SetExclusiveStartStreamName(const char* value) { m_exclusiveStartStreamNameHasBeenSet = true; m_exclusiveStartStreamName.assign(value); }
    inline ListStreamsRequest& WithExclusiveStartStreamName(const Aws::String& value) { SetExclusiveStartStreamName(value); return *this;}
    inline ListStreamsRequest& WithExclusiveStartStreamName(Aws::String&& value) { SetExclusiveStartStreamName(std::move(value)); return *this;}
    inline ListStreamsRequest& WithExclusiveStartStreamName(const char* value) { SetExclusiveStartStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p/>
     */
    inline const Aws::String& GetNextToken() const{ return m_nextToken; }
    inline bool NextTokenHasBeenSet() const { return m_nextTokenHasBeenSet; }
    inline void SetNextToken(const Aws::String& value) { m_nextTokenHasBeenSet = true; m_nextToken = value; }
    inline void SetNextToken(Aws::String&& value) { m_nextTokenHasBeenSet = true; m_nextToken = std::move(value); }
    inline void SetNextToken(const char* value) { m_nextTokenHasBeenSet = true; m_nextToken.assign(value); }
    inline ListStreamsRequest& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline ListStreamsRequest& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline ListStreamsRequest& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}
  private:

    int m_limit;
    bool m_limitHasBeenSet = false;

    Aws::String m_exclusiveStartStreamName;
    bool m_exclusiveStartStreamNameHasBeenSet = false;

    Aws::String m_nextToken;
    bool m_nextTokenHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
