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
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace CognitoIdentity
{
namespace Model
{

  /**
   * <p>A provider representing an Amazon Cognito user pool and its client
   * ID.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/CognitoIdentityProvider">AWS
   * API Reference</a></p>
   */
  class CognitoIdentityProvider
  {
  public:
    AWS_COGNITOIDENTITY_API CognitoIdentityProvider();
    AWS_COGNITOIDENTITY_API CognitoIdentityProvider(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API CognitoIdentityProvider& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The provider name for an Amazon Cognito user pool. For example,
     * <code>cognito-idp.us-east-1.amazonaws.com/us-east-1_123456789</code>.</p>
     */
    inline const Aws::String& GetProviderName() const{ return m_providerName; }
    inline bool ProviderNameHasBeenSet() const { return m_providerNameHasBeenSet; }
    inline void SetProviderName(const Aws::String& value) { m_providerNameHasBeenSet = true; m_providerName = value; }
    inline void SetProviderName(Aws::String&& value) { m_providerNameHasBeenSet = true; m_providerName = std::move(value); }
    inline void SetProviderName(const char* value) { m_providerNameHasBeenSet = true; m_providerName.assign(value); }
    inline CognitoIdentityProvider& WithProviderName(const Aws::String& value) { SetProviderName(value); return *this;}
    inline CognitoIdentityProvider& WithProviderName(Aws::String&& value) { SetProviderName(std::move(value)); return *this;}
    inline CognitoIdentityProvider& WithProviderName(const char* value) { SetProviderName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The client ID for the Amazon Cognito user pool.</p>
     */
    inline const Aws::String& GetClientId() const{ return m_clientId; }
    inline bool ClientIdHasBeenSet() const { return m_clientIdHasBeenSet; }
    inline void SetClientId(const Aws::String& value) { m_clientIdHasBeenSet = true; m_clientId = value; }
    inline void SetClientId(Aws::String&& value) { m_clientIdHasBeenSet = true; m_clientId = std::move(value); }
    inline void SetClientId(const char* value) { m_clientIdHasBeenSet = true; m_clientId.assign(value); }
    inline CognitoIdentityProvider& WithClientId(const Aws::String& value) { SetClientId(value); return *this;}
    inline CognitoIdentityProvider& WithClientId(Aws::String&& value) { SetClientId(std::move(value)); return *this;}
    inline CognitoIdentityProvider& WithClientId(const char* value) { SetClientId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>TRUE if server-side token validation is enabled for the identity provider’s
     * token.</p> <p>Once you set <code>ServerSideTokenCheck</code> to TRUE for an
     * identity pool, that identity pool will check with the integrated user pools to
     * make sure that the user has not been globally signed out or deleted before the
     * identity pool provides an OIDC token or AWS credentials for the user.</p> <p>If
     * the user is signed out or deleted, the identity pool will return a 400 Not
     * Authorized error.</p>
     */
    inline bool GetServerSideTokenCheck() const{ return m_serverSideTokenCheck; }
    inline bool ServerSideTokenCheckHasBeenSet() const { return m_serverSideTokenCheckHasBeenSet; }
    inline void SetServerSideTokenCheck(bool value) { m_serverSideTokenCheckHasBeenSet = true; m_serverSideTokenCheck = value; }
    inline CognitoIdentityProvider& WithServerSideTokenCheck(bool value) { SetServerSideTokenCheck(value); return *this;}
    ///@}
  private:

    Aws::String m_providerName;
    bool m_providerNameHasBeenSet = false;

    Aws::String m_clientId;
    bool m_clientIdHasBeenSet = false;

    bool m_serverSideTokenCheck;
    bool m_serverSideTokenCheckHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
