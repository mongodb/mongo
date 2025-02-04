/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/KinesisRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <utility>

namespace Aws
{
namespace Kinesis
{
namespace Model
{

  /**
   */
  class ListStreamConsumersRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API ListStreamConsumersRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListStreamConsumers"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The ARN of the Kinesis data stream for which you want to list the registered
     * consumers. For more information, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html#arn-syntax-kinesis-streams">Amazon
     * Resource Names (ARNs) and Amazon Web Services Service Namespaces</a>.</p>
     */
    inline const Aws::String& GetStreamARN() const{ return m_streamARN; }
    inline bool StreamARNHasBeenSet() const { return m_streamARNHasBeenSet; }
    inline void SetStreamARN(const Aws::String& value) { m_streamARNHasBeenSet = true; m_streamARN = value; }
    inline void SetStreamARN(Aws::String&& value) { m_streamARNHasBeenSet = true; m_streamARN = std::move(value); }
    inline void SetStreamARN(const char* value) { m_streamARNHasBeenSet = true; m_streamARN.assign(value); }
    inline ListStreamConsumersRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline ListStreamConsumersRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline ListStreamConsumersRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When the number of consumers that are registered with the data stream is
     * greater than the default value for the <code>MaxResults</code> parameter, or if
     * you explicitly specify a value for <code>MaxResults</code> that is less than the
     * number of consumers that are registered with the data stream, the response
     * includes a pagination token named <code>NextToken</code>. You can specify this
     * <code>NextToken</code> value in a subsequent call to
     * <code>ListStreamConsumers</code> to list the next set of registered
     * consumers.</p> <p>Don't specify <code>StreamName</code> or
     * <code>StreamCreationTimestamp</code> if you specify <code>NextToken</code>
     * because the latter unambiguously identifies the stream.</p> <p>You can
     * optionally specify a value for the <code>MaxResults</code> parameter when you
     * specify <code>NextToken</code>. If you specify a <code>MaxResults</code> value
     * that is less than the number of consumers that the operation returns if you
     * don't specify <code>MaxResults</code>, the response will contain a new
     * <code>NextToken</code> value. You can use the new <code>NextToken</code> value
     * in a subsequent call to the <code>ListStreamConsumers</code> operation to list
     * the next set of consumers.</p>  <p>Tokens expire after 300 seconds.
     * When you obtain a value for <code>NextToken</code> in the response to a call to
     * <code>ListStreamConsumers</code>, you have 300 seconds to use that value. If you
     * specify an expired token in a call to <code>ListStreamConsumers</code>, you get
     * <code>ExpiredNextTokenException</code>.</p> 
     */
    inline const Aws::String& GetNextToken() const{ return m_nextToken; }
    inline bool NextTokenHasBeenSet() const { return m_nextTokenHasBeenSet; }
    inline void SetNextToken(const Aws::String& value) { m_nextTokenHasBeenSet = true; m_nextToken = value; }
    inline void SetNextToken(Aws::String&& value) { m_nextTokenHasBeenSet = true; m_nextToken = std::move(value); }
    inline void SetNextToken(const char* value) { m_nextTokenHasBeenSet = true; m_nextToken.assign(value); }
    inline ListStreamConsumersRequest& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline ListStreamConsumersRequest& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline ListStreamConsumersRequest& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of consumers that you want a single call of
     * <code>ListStreamConsumers</code> to return. The default value is 100. If you
     * specify a value greater than 100, at most 100 results are returned. </p>
     */
    inline int GetMaxResults() const{ return m_maxResults; }
    inline bool MaxResultsHasBeenSet() const { return m_maxResultsHasBeenSet; }
    inline void SetMaxResults(int value) { m_maxResultsHasBeenSet = true; m_maxResults = value; }
    inline ListStreamConsumersRequest& WithMaxResults(int value) { SetMaxResults(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specify this input parameter to distinguish data streams that have the same
     * name. For example, if you create a data stream and then delete it, and you later
     * create another data stream with the same name, you can use this input parameter
     * to specify which of the two streams you want to list the consumers for. </p>
     * <p>You can't specify this parameter if you specify the NextToken parameter. </p>
     */
    inline const Aws::Utils::DateTime& GetStreamCreationTimestamp() const{ return m_streamCreationTimestamp; }
    inline bool StreamCreationTimestampHasBeenSet() const { return m_streamCreationTimestampHasBeenSet; }
    inline void SetStreamCreationTimestamp(const Aws::Utils::DateTime& value) { m_streamCreationTimestampHasBeenSet = true; m_streamCreationTimestamp = value; }
    inline void SetStreamCreationTimestamp(Aws::Utils::DateTime&& value) { m_streamCreationTimestampHasBeenSet = true; m_streamCreationTimestamp = std::move(value); }
    inline ListStreamConsumersRequest& WithStreamCreationTimestamp(const Aws::Utils::DateTime& value) { SetStreamCreationTimestamp(value); return *this;}
    inline ListStreamConsumersRequest& WithStreamCreationTimestamp(Aws::Utils::DateTime&& value) { SetStreamCreationTimestamp(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;

    Aws::String m_nextToken;
    bool m_nextTokenHasBeenSet = false;

    int m_maxResults;
    bool m_maxResultsHasBeenSet = false;

    Aws::Utils::DateTime m_streamCreationTimestamp;
    bool m_streamCreationTimestampHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
