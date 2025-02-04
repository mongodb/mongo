/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/model/Consumer.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Kinesis
{
namespace Model
{
  class ListStreamConsumersResult
  {
  public:
    AWS_KINESIS_API ListStreamConsumersResult();
    AWS_KINESIS_API ListStreamConsumersResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_KINESIS_API ListStreamConsumersResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>An array of JSON objects. Each object represents one registered consumer.</p>
     */
    inline const Aws::Vector<Consumer>& GetConsumers() const{ return m_consumers; }
    inline void SetConsumers(const Aws::Vector<Consumer>& value) { m_consumers = value; }
    inline void SetConsumers(Aws::Vector<Consumer>&& value) { m_consumers = std::move(value); }
    inline ListStreamConsumersResult& WithConsumers(const Aws::Vector<Consumer>& value) { SetConsumers(value); return *this;}
    inline ListStreamConsumersResult& WithConsumers(Aws::Vector<Consumer>&& value) { SetConsumers(std::move(value)); return *this;}
    inline ListStreamConsumersResult& AddConsumers(const Consumer& value) { m_consumers.push_back(value); return *this; }
    inline ListStreamConsumersResult& AddConsumers(Consumer&& value) { m_consumers.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>When the number of consumers that are registered with the data stream is
     * greater than the default value for the <code>MaxResults</code> parameter, or if
     * you explicitly specify a value for <code>MaxResults</code> that is less than the
     * number of registered consumers, the response includes a pagination token named
     * <code>NextToken</code>. You can specify this <code>NextToken</code> value in a
     * subsequent call to <code>ListStreamConsumers</code> to list the next set of
     * registered consumers. For more information about the use of this pagination
     * token when calling the <code>ListStreamConsumers</code> operation, see
     * <a>ListStreamConsumersInput$NextToken</a>.</p>  <p>Tokens expire
     * after 300 seconds. When you obtain a value for <code>NextToken</code> in the
     * response to a call to <code>ListStreamConsumers</code>, you have 300 seconds to
     * use that value. If you specify an expired token in a call to
     * <code>ListStreamConsumers</code>, you get
     * <code>ExpiredNextTokenException</code>.</p> 
     */
    inline const Aws::String& GetNextToken() const{ return m_nextToken; }
    inline void SetNextToken(const Aws::String& value) { m_nextToken = value; }
    inline void SetNextToken(Aws::String&& value) { m_nextToken = std::move(value); }
    inline void SetNextToken(const char* value) { m_nextToken.assign(value); }
    inline ListStreamConsumersResult& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline ListStreamConsumersResult& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline ListStreamConsumersResult& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListStreamConsumersResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListStreamConsumersResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListStreamConsumersResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<Consumer> m_consumers;

    Aws::String m_nextToken;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
