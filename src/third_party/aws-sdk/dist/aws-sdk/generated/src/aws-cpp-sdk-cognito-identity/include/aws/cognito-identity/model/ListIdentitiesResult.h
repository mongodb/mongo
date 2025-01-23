/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/cognito-identity/model/IdentityDescription.h>
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
   * <p>The response to a ListIdentities request.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/ListIdentitiesResponse">AWS
   * API Reference</a></p>
   */
  class ListIdentitiesResult
  {
  public:
    AWS_COGNITOIDENTITY_API ListIdentitiesResult();
    AWS_COGNITOIDENTITY_API ListIdentitiesResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_COGNITOIDENTITY_API ListIdentitiesResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>An identity pool ID in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityPoolId() const{ return m_identityPoolId; }
    inline void SetIdentityPoolId(const Aws::String& value) { m_identityPoolId = value; }
    inline void SetIdentityPoolId(Aws::String&& value) { m_identityPoolId = std::move(value); }
    inline void SetIdentityPoolId(const char* value) { m_identityPoolId.assign(value); }
    inline ListIdentitiesResult& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline ListIdentitiesResult& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline ListIdentitiesResult& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An object containing a set of identities and associated mappings.</p>
     */
    inline const Aws::Vector<IdentityDescription>& GetIdentities() const{ return m_identities; }
    inline void SetIdentities(const Aws::Vector<IdentityDescription>& value) { m_identities = value; }
    inline void SetIdentities(Aws::Vector<IdentityDescription>&& value) { m_identities = std::move(value); }
    inline ListIdentitiesResult& WithIdentities(const Aws::Vector<IdentityDescription>& value) { SetIdentities(value); return *this;}
    inline ListIdentitiesResult& WithIdentities(Aws::Vector<IdentityDescription>&& value) { SetIdentities(std::move(value)); return *this;}
    inline ListIdentitiesResult& AddIdentities(const IdentityDescription& value) { m_identities.push_back(value); return *this; }
    inline ListIdentitiesResult& AddIdentities(IdentityDescription&& value) { m_identities.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>A pagination token.</p>
     */
    inline const Aws::String& GetNextToken() const{ return m_nextToken; }
    inline void SetNextToken(const Aws::String& value) { m_nextToken = value; }
    inline void SetNextToken(Aws::String&& value) { m_nextToken = std::move(value); }
    inline void SetNextToken(const char* value) { m_nextToken.assign(value); }
    inline ListIdentitiesResult& WithNextToken(const Aws::String& value) { SetNextToken(value); return *this;}
    inline ListIdentitiesResult& WithNextToken(Aws::String&& value) { SetNextToken(std::move(value)); return *this;}
    inline ListIdentitiesResult& WithNextToken(const char* value) { SetNextToken(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListIdentitiesResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListIdentitiesResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListIdentitiesResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_identityPoolId;

    Aws::Vector<IdentityDescription> m_identities;

    Aws::String m_nextToken;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
