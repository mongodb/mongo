/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/cognito-identity/model/IdentityPoolShortDescription.h>
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
namespace CognitoIdentity
{
namespace Model
{
  /**
   * <p>The result of a successful ListIdentityPools action.</p><p><h3>See Also:</h3>
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/ListIdentityPoolsResponse">AWS
   * API Reference</a></p>
   */
  class ListIdentityPoolsResult
  {
  public:
    AWS_COGNITOIDENTITY_API ListIdentityPoolsResult();
    AWS_COGNITOIDENTITY_API ListIdentityPoolsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_COGNITOIDENTITY_API ListIdentityPoolsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The identity pools returned by the ListIdentityPools action.</p>
     */
    inline const Aws::Vector<IdentityPoolShortDescription>& GetIdentityPools() const{ return m_identityPools; }
    inline void SetIdentityPools(const Aws::Vector<IdentityPoolShortDescription>& value) { m_identityPools = value; }
    inline void SetIdentityPools(Aws::Vector<IdentityPoolShortDescription>&& value) { m_identityPools = std::move(value); }
    inline ListIdentityPoolsResult& WithIdentityPools(const Aws::Vector<IdentityPoolShortDescription>& value) { SetIdentityPools(value); return *this;}
    inline ListIdentityPoolsResult& WithIdentityPools(Aws::Vector<IdentityPoolShortDescription>&& value) { SetIdentityPools(std::move(value)); return *this;}
    inline ListIdentityPoolsResult& AddIdentityPools(const IdentityPoolShortDescription& value) { m_identityPools.push_back(value); return *this; }
    inline ListIdentityPoolsResult& AddIdentityPools(IdentityPoolShortDescription&& value) { m_identityPools.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>A pagination token.</p>
     */
    inline const Aws::String& GetNextToken() const{ return m_nextToken; }
    inline void SetNextToken(const Aws::String& value) { m_nextToken = value; }
    inline void SetNextToken(Aws::String&& value) { m_nextToken = std::move(value); }
    inline void SetNextToken(const char* value) { m_nextToken.assign(value); }
    inline ListIdentityPoolsResult& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline ListIdentityPoolsResult& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline ListIdentityPoolsResult& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListIdentityPoolsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListIdentityPoolsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListIdentityPoolsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<IdentityPoolShortDescription> m_identityPools;

    Aws::String m_nextToken;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
