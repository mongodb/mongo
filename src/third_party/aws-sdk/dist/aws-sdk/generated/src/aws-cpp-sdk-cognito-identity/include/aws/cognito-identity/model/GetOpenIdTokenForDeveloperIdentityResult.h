/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Returned in response to a successful
   * <code>GetOpenIdTokenForDeveloperIdentity</code> request.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/GetOpenIdTokenForDeveloperIdentityResponse">AWS
   * API Reference</a></p>
   */
  class GetOpenIdTokenForDeveloperIdentityResult
  {
  public:
    AWS_COGNITOIDENTITY_API GetOpenIdTokenForDeveloperIdentityResult();
    AWS_COGNITOIDENTITY_API GetOpenIdTokenForDeveloperIdentityResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_COGNITOIDENTITY_API GetOpenIdTokenForDeveloperIdentityResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A unique identifier in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityId() const{ return m_identityId; }
    inline void SetIdentityId(const Aws::String& value) { m_identityId = value; }
    inline void SetIdentityId(Aws::String&& value) { m_identityId = std::move(value); }
    inline void SetIdentityId(const char* value) { m_identityId.assign(value); }
    inline GetOpenIdTokenForDeveloperIdentityResult& WithIdentityId(const Aws::String& value) { SetIdentityId(value); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityResult& WithIdentityId(Aws::String&& value) { SetIdentityId(std::move(value)); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityResult& WithIdentityId(const char* value) { SetIdentityId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An OpenID token.</p>
     */
    inline const Aws::String& GetToken() const{ return m_token; }
    inline void SetToken(const Aws::String& value) { m_token = value; }
    inline void SetToken(Aws::String&& value) { m_token = std::move(value); }
    inline void SetToken(const char* value) { m_token.assign(value); }
    inline GetOpenIdTokenForDeveloperIdentityResult& WithToken(const Aws::String& value) { SetToken(value); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityResult& WithToken(Aws::String&& value) { SetToken(std::move(value)); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityResult& WithToken(const char* value) { SetToken(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetOpenIdTokenForDeveloperIdentityResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_identityId;

    Aws::String m_token;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
