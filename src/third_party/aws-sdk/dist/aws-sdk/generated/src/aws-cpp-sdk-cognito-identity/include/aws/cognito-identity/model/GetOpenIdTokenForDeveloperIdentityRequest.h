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
   * <p>Input to the <code>GetOpenIdTokenForDeveloperIdentity</code>
   * action.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/GetOpenIdTokenForDeveloperIdentityInput">AWS
   * API Reference</a></p>
   */
  class GetOpenIdTokenForDeveloperIdentityRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API GetOpenIdTokenForDeveloperIdentityRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetOpenIdTokenForDeveloperIdentity"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>An identity pool ID in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityPoolId() const{ return m_identityPoolId; }
    inline bool IdentityPoolIdHasBeenSet() const { return m_identityPoolIdHasBeenSet; }
    inline void SetIdentityPoolId(const Aws::String& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = value; }
    inline void SetIdentityPoolId(Aws::String&& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = std::move(value); }
    inline void SetIdentityPoolId(const char* value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId.assign(value); }
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A unique identifier in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityId() const{ return m_identityId; }
    inline bool IdentityIdHasBeenSet() const { return m_identityIdHasBeenSet; }
    inline void SetIdentityId(const Aws::String& value) { m_identityIdHasBeenSet = true; m_identityId = value; }
    inline void SetIdentityId(Aws::String&& value) { m_identityIdHasBeenSet = true; m_identityId = std::move(value); }
    inline void SetIdentityId(const char* value) { m_identityIdHasBeenSet = true; m_identityId.assign(value); }
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithIdentityId(const Aws::String& value) { SetIdentityId(value); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithIdentityId(Aws::String&& value) { SetIdentityId(std::move(value)); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithIdentityId(const char* value) { SetIdentityId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A set of optional name-value pairs that map provider names to provider
     * tokens. Each name-value pair represents a user from a public provider or
     * developer provider. If the user is from a developer provider, the name-value
     * pair will follow the syntax <code>"developer_provider_name":
     * "developer_user_identifier"</code>. The developer provider is the "domain" by
     * which Cognito will refer to your users; you provided this domain while
     * creating/updating the identity pool. The developer user identifier is an
     * identifier from your backend that uniquely identifies a user. When you create an
     * identity pool, you can specify the supported logins.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetLogins() const{ return m_logins; }
    inline bool LoginsHasBeenSet() const { return m_loginsHasBeenSet; }
    inline void SetLogins(const Aws::Map<Aws::String, Aws::String>& value) { m_loginsHasBeenSet = true; m_logins = value; }
    inline void SetLogins(Aws::Map<Aws::String, Aws::String>&& value) { m_loginsHasBeenSet = true; m_logins = std::move(value); }
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithLogins(const Aws::Map<Aws::String, Aws::String>& value) { SetLogins(value); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithLogins(Aws::Map<Aws::String, Aws::String>&& value) { SetLogins(std::move(value)); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddLogins(const Aws::String& key, const Aws::String& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, value); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddLogins(Aws::String&& key, const Aws::String& value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), value); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddLogins(const Aws::String& key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, std::move(value)); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddLogins(Aws::String&& key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), std::move(value)); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddLogins(const char* key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, std::move(value)); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddLogins(Aws::String&& key, const char* value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), value); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddLogins(const char* key, const char* value) { m_loginsHasBeenSet = true; m_logins.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>Use this operation to configure attribute mappings for custom providers. </p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetPrincipalTags() const{ return m_principalTags; }
    inline bool PrincipalTagsHasBeenSet() const { return m_principalTagsHasBeenSet; }
    inline void SetPrincipalTags(const Aws::Map<Aws::String, Aws::String>& value) { m_principalTagsHasBeenSet = true; m_principalTags = value; }
    inline void SetPrincipalTags(Aws::Map<Aws::String, Aws::String>&& value) { m_principalTagsHasBeenSet = true; m_principalTags = std::move(value); }
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithPrincipalTags(const Aws::Map<Aws::String, Aws::String>& value) { SetPrincipalTags(value); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithPrincipalTags(Aws::Map<Aws::String, Aws::String>&& value) { SetPrincipalTags(std::move(value)); return *this;}
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddPrincipalTags(const Aws::String& key, const Aws::String& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(key, value); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddPrincipalTags(Aws::String&& key, const Aws::String& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(std::move(key), value); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddPrincipalTags(const Aws::String& key, Aws::String&& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(key, std::move(value)); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddPrincipalTags(Aws::String&& key, Aws::String&& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(std::move(key), std::move(value)); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddPrincipalTags(const char* key, Aws::String&& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(key, std::move(value)); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddPrincipalTags(Aws::String&& key, const char* value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(std::move(key), value); return *this; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& AddPrincipalTags(const char* key, const char* value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The expiration time of the token, in seconds. You can specify a custom
     * expiration time for the token so that you can cache it. If you don't provide an
     * expiration time, the token is valid for 15 minutes. You can exchange the token
     * with Amazon STS for temporary AWS credentials, which are valid for a maximum of
     * one hour. The maximum token duration you can set is 24 hours. You should take
     * care in setting the expiration time for a token, as there are significant
     * security implications: an attacker could use a leaked token to access your AWS
     * resources for the token's duration.</p>  <p>Please provide for a small
     * grace period, usually no more than 5 minutes, to account for clock skew.</p>
     * 
     */
    inline long long GetTokenDuration() const{ return m_tokenDuration; }
    inline bool TokenDurationHasBeenSet() const { return m_tokenDurationHasBeenSet; }
    inline void SetTokenDuration(long long value) { m_tokenDurationHasBeenSet = true; m_tokenDuration = value; }
    inline GetOpenIdTokenForDeveloperIdentityRequest& WithTokenDuration(long long value) { SetTokenDuration(value); return *this;}
    ///@}
  private:

    Aws::String m_identityPoolId;
    bool m_identityPoolIdHasBeenSet = false;

    Aws::String m_identityId;
    bool m_identityIdHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_logins;
    bool m_loginsHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_principalTags;
    bool m_principalTagsHasBeenSet = false;

    long long m_tokenDuration;
    bool m_tokenDurationHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
