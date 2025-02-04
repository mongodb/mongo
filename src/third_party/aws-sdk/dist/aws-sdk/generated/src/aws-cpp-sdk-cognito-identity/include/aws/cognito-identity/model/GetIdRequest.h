/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/cognito-identity/CognitoIdentityRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <utility>

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

  /**
   * <p>Input to the GetId action.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/GetIdInput">AWS
   * API Reference</a></p>
   */
  class GetIdRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API GetIdRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetId"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>A standard AWS account ID (9+ digits).</p>
     */
    inline const Aws::String& GetAccountId() const{ return m_accountId; }
    inline bool AccountIdHasBeenSet() const { return m_accountIdHasBeenSet; }
    inline void SetAccountId(const Aws::String& value) { m_accountIdHasBeenSet = true; m_accountId = value; }
    inline void SetAccountId(Aws::String&& value) { m_accountIdHasBeenSet = true; m_accountId = std::move(value); }
    inline void SetAccountId(const char* value) { m_accountIdHasBeenSet = true; m_accountId.assign(value); }
    inline GetIdRequest& WithAccountId(const Aws::String& value) { SetAccountId(value); return *this;}
    inline GetIdRequest& WithAccountId(Aws::String&& value) { SetAccountId(std::move(value)); return *this;}
    inline GetIdRequest& WithAccountId(const char* value) { SetAccountId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An identity pool ID in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityPoolId() const{ return m_identityPoolId; }
    inline bool IdentityPoolIdHasBeenSet() const { return m_identityPoolIdHasBeenSet; }
    inline void SetIdentityPoolId(const Aws::String& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = value; }
    inline void SetIdentityPoolId(Aws::String&& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = std::move(value); }
    inline void SetIdentityPoolId(const char* value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId.assign(value); }
    inline GetIdRequest& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline GetIdRequest& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline GetIdRequest& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A set of optional name-value pairs that map provider names to provider
     * tokens. The available provider names for <code>Logins</code> are as follows:</p>
     * <ul> <li> <p>Facebook: <code>graph.facebook.com</code> </p> </li> <li> <p>Amazon
     * Cognito user pool:
     * <code>cognito-idp.&lt;region&gt;.amazonaws.com/&lt;YOUR_USER_POOL_ID&gt;</code>,
     * for example,
     * <code>cognito-idp.us-east-1.amazonaws.com/us-east-1_123456789</code>. </p> </li>
     * <li> <p>Google: <code>accounts.google.com</code> </p> </li> <li> <p>Amazon:
     * <code>www.amazon.com</code> </p> </li> <li> <p>Twitter:
     * <code>api.twitter.com</code> </p> </li> <li> <p>Digits:
     * <code>www.digits.com</code> </p> </li> </ul>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetLogins() const{ return m_logins; }
    inline bool LoginsHasBeenSet() const { return m_loginsHasBeenSet; }
    inline void SetLogins(const Aws::Map<Aws::String, Aws::String>& value) { m_loginsHasBeenSet = true; m_logins = value; }
    inline void SetLogins(Aws::Map<Aws::String, Aws::String>&& value) { m_loginsHasBeenSet = true; m_logins = std::move(value); }
    inline GetIdRequest& WithLogins(const Aws::Map<Aws::String, Aws::String>& value) { SetLogins(value); return *this;}
    inline GetIdRequest& WithLogins(Aws::Map<Aws::String, Aws::String>&& value) { SetLogins(std::move(value)); return *this;}
    inline GetIdRequest& AddLogins(const Aws::String& key, const Aws::String& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, value); return *this; }
    inline GetIdRequest& AddLogins(Aws::String&& key, const Aws::String& value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), value); return *this; }
    inline GetIdRequest& AddLogins(const Aws::String& key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, std::move(value)); return *this; }
    inline GetIdRequest& AddLogins(Aws::String&& key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), std::move(value)); return *this; }
    inline GetIdRequest& AddLogins(const char* key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, std::move(value)); return *this; }
    inline GetIdRequest& AddLogins(Aws::String&& key, const char* value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), value); return *this; }
    inline GetIdRequest& AddLogins(const char* key, const char* value) { m_loginsHasBeenSet = true; m_logins.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_accountId;
    bool m_accountIdHasBeenSet = false;

    Aws::String m_identityPoolId;
    bool m_identityPoolIdHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_logins;
    bool m_loginsHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
