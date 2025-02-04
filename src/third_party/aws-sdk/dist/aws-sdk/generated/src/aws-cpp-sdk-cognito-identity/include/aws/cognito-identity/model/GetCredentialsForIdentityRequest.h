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
   * <p>Input to the <code>GetCredentialsForIdentity</code> action.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/GetCredentialsForIdentityInput">AWS
   * API Reference</a></p>
   */
  class GetCredentialsForIdentityRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API GetCredentialsForIdentityRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetCredentialsForIdentity"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>A unique identifier in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityId() const{ return m_identityId; }
    inline bool IdentityIdHasBeenSet() const { return m_identityIdHasBeenSet; }
    inline void SetIdentityId(const Aws::String& value) { m_identityIdHasBeenSet = true; m_identityId = value; }
    inline void SetIdentityId(Aws::String&& value) { m_identityIdHasBeenSet = true; m_identityId = std::move(value); }
    inline void SetIdentityId(const char* value) { m_identityIdHasBeenSet = true; m_identityId.assign(value); }
    inline GetCredentialsForIdentityRequest& WithIdentityId(const Aws::String& value) { SetIdentityId(value); return *this;}
    inline GetCredentialsForIdentityRequest& WithIdentityId(Aws::String&& value) { SetIdentityId(std::move(value)); return *this;}
    inline GetCredentialsForIdentityRequest& WithIdentityId(const char* value) { SetIdentityId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A set of optional name-value pairs that map provider names to provider
     * tokens. The name-value pair will follow the syntax "provider_name":
     * "provider_user_identifier".</p> <p>Logins should not be specified when trying to
     * get credentials for an unauthenticated identity.</p> <p>The Logins parameter is
     * required when using identities associated with external identity providers such
     * as Facebook. For examples of <code>Logins</code> maps, see the code examples in
     * the <a
     * href="https://docs.aws.amazon.com/cognito/latest/developerguide/external-identity-providers.html">External
     * Identity Providers</a> section of the Amazon Cognito Developer Guide.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetLogins() const{ return m_logins; }
    inline bool LoginsHasBeenSet() const { return m_loginsHasBeenSet; }
    inline void SetLogins(const Aws::Map<Aws::String, Aws::String>& value) { m_loginsHasBeenSet = true; m_logins = value; }
    inline void SetLogins(Aws::Map<Aws::String, Aws::String>&& value) { m_loginsHasBeenSet = true; m_logins = std::move(value); }
    inline GetCredentialsForIdentityRequest& WithLogins(const Aws::Map<Aws::String, Aws::String>& value) { SetLogins(value); return *this;}
    inline GetCredentialsForIdentityRequest& WithLogins(Aws::Map<Aws::String, Aws::String>&& value) { SetLogins(std::move(value)); return *this;}
    inline GetCredentialsForIdentityRequest& AddLogins(const Aws::String& key, const Aws::String& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, value); return *this; }
    inline GetCredentialsForIdentityRequest& AddLogins(Aws::String&& key, const Aws::String& value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), value); return *this; }
    inline GetCredentialsForIdentityRequest& AddLogins(const Aws::String& key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, std::move(value)); return *this; }
    inline GetCredentialsForIdentityRequest& AddLogins(Aws::String&& key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), std::move(value)); return *this; }
    inline GetCredentialsForIdentityRequest& AddLogins(const char* key, Aws::String&& value) { m_loginsHasBeenSet = true; m_logins.emplace(key, std::move(value)); return *this; }
    inline GetCredentialsForIdentityRequest& AddLogins(Aws::String&& key, const char* value) { m_loginsHasBeenSet = true; m_logins.emplace(std::move(key), value); return *this; }
    inline GetCredentialsForIdentityRequest& AddLogins(const char* key, const char* value) { m_loginsHasBeenSet = true; m_logins.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the role to be assumed when multiple roles
     * were received in the token from the identity provider. For example, a SAML-based
     * identity provider. This parameter is optional for identity providers that do not
     * support role customization.</p>
     */
    inline const Aws::String& GetCustomRoleArn() const{ return m_customRoleArn; }
    inline bool CustomRoleArnHasBeenSet() const { return m_customRoleArnHasBeenSet; }
    inline void SetCustomRoleArn(const Aws::String& value) { m_customRoleArnHasBeenSet = true; m_customRoleArn = value; }
    inline void SetCustomRoleArn(Aws::String&& value) { m_customRoleArnHasBeenSet = true; m_customRoleArn = std::move(value); }
    inline void SetCustomRoleArn(const char* value) { m_customRoleArnHasBeenSet = true; m_customRoleArn.assign(value); }
    inline GetCredentialsForIdentityRequest& WithCustomRoleArn(const Aws::String& value) { SetCustomRoleArn(value); return *this;}
    inline GetCredentialsForIdentityRequest& WithCustomRoleArn(Aws::String&& value) { SetCustomRoleArn(std::move(value)); return *this;}
    inline GetCredentialsForIdentityRequest& WithCustomRoleArn(const char* value) { SetCustomRoleArn(value); return *this;}
    ///@}
  private:

    Aws::String m_identityId;
    bool m_identityIdHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_logins;
    bool m_loginsHasBeenSet = false;

    Aws::String m_customRoleArn;
    bool m_customRoleArnHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
