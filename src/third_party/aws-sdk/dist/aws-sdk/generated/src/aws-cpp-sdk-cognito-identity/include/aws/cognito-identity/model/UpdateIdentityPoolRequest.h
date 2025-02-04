/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/cognito-identity/CognitoIdentityRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/cognito-identity/model/CognitoIdentityProvider.h>
#include <utility>

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

  /**
   * <p>An object representing an Amazon Cognito identity pool.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/IdentityPool">AWS
   * API Reference</a></p>
   */
  class UpdateIdentityPoolRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API UpdateIdentityPoolRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateIdentityPool"; }

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
    inline UpdateIdentityPoolRequest& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline UpdateIdentityPoolRequest& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A string that you provide.</p>
     */
    inline const Aws::String& GetIdentityPoolName() const{ return m_identityPoolName; }
    inline bool IdentityPoolNameHasBeenSet() const { return m_identityPoolNameHasBeenSet; }
    inline void SetIdentityPoolName(const Aws::String& value) { m_identityPoolNameHasBeenSet = true; m_identityPoolName = value; }
    inline void SetIdentityPoolName(Aws::String&& value) { m_identityPoolNameHasBeenSet = true; m_identityPoolName = std::move(value); }
    inline void SetIdentityPoolName(const char* value) { m_identityPoolNameHasBeenSet = true; m_identityPoolName.assign(value); }
    inline UpdateIdentityPoolRequest& WithIdentityPoolName(const Aws::String& value) { SetIdentityPoolName(value); return *this;}
    inline UpdateIdentityPoolRequest& WithIdentityPoolName(Aws::String&& value) { SetIdentityPoolName(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& WithIdentityPoolName(const char* value) { SetIdentityPoolName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>TRUE if the identity pool supports unauthenticated logins.</p>
     */
    inline bool GetAllowUnauthenticatedIdentities() const{ return m_allowUnauthenticatedIdentities; }
    inline bool AllowUnauthenticatedIdentitiesHasBeenSet() const { return m_allowUnauthenticatedIdentitiesHasBeenSet; }
    inline void SetAllowUnauthenticatedIdentities(bool value) { m_allowUnauthenticatedIdentitiesHasBeenSet = true; m_allowUnauthenticatedIdentities = value; }
    inline UpdateIdentityPoolRequest& WithAllowUnauthenticatedIdentities(bool value) { SetAllowUnauthenticatedIdentities(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Enables or disables the Basic (Classic) authentication flow. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/cognito/latest/developerguide/authentication-flow.html">Identity
     * Pools (Federated Identities) Authentication Flow</a> in the <i>Amazon Cognito
     * Developer Guide</i>.</p>
     */
    inline bool GetAllowClassicFlow() const{ return m_allowClassicFlow; }
    inline bool AllowClassicFlowHasBeenSet() const { return m_allowClassicFlowHasBeenSet; }
    inline void SetAllowClassicFlow(bool value) { m_allowClassicFlowHasBeenSet = true; m_allowClassicFlow = value; }
    inline UpdateIdentityPoolRequest& WithAllowClassicFlow(bool value) { SetAllowClassicFlow(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Optional key:value pairs mapping provider names to provider app IDs.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetSupportedLoginProviders() const{ return m_supportedLoginProviders; }
    inline bool SupportedLoginProvidersHasBeenSet() const { return m_supportedLoginProvidersHasBeenSet; }
    inline void SetSupportedLoginProviders(const Aws::Map<Aws::String, Aws::String>& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders = value; }
    inline void SetSupportedLoginProviders(Aws::Map<Aws::String, Aws::String>&& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders = std::move(value); }
    inline UpdateIdentityPoolRequest& WithSupportedLoginProviders(const Aws::Map<Aws::String, Aws::String>& value) { SetSupportedLoginProviders(value); return *this;}
    inline UpdateIdentityPoolRequest& WithSupportedLoginProviders(Aws::Map<Aws::String, Aws::String>&& value) { SetSupportedLoginProviders(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& AddSupportedLoginProviders(const Aws::String& key, const Aws::String& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(key, value); return *this; }
    inline UpdateIdentityPoolRequest& AddSupportedLoginProviders(Aws::String&& key, const Aws::String& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(std::move(key), value); return *this; }
    inline UpdateIdentityPoolRequest& AddSupportedLoginProviders(const Aws::String& key, Aws::String&& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(key, std::move(value)); return *this; }
    inline UpdateIdentityPoolRequest& AddSupportedLoginProviders(Aws::String&& key, Aws::String&& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(std::move(key), std::move(value)); return *this; }
    inline UpdateIdentityPoolRequest& AddSupportedLoginProviders(const char* key, Aws::String&& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(key, std::move(value)); return *this; }
    inline UpdateIdentityPoolRequest& AddSupportedLoginProviders(Aws::String&& key, const char* value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(std::move(key), value); return *this; }
    inline UpdateIdentityPoolRequest& AddSupportedLoginProviders(const char* key, const char* value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The "domain" by which Cognito will refer to your users.</p>
     */
    inline const Aws::String& GetDeveloperProviderName() const{ return m_developerProviderName; }
    inline bool DeveloperProviderNameHasBeenSet() const { return m_developerProviderNameHasBeenSet; }
    inline void SetDeveloperProviderName(const Aws::String& value) { m_developerProviderNameHasBeenSet = true; m_developerProviderName = value; }
    inline void SetDeveloperProviderName(Aws::String&& value) { m_developerProviderNameHasBeenSet = true; m_developerProviderName = std::move(value); }
    inline void SetDeveloperProviderName(const char* value) { m_developerProviderNameHasBeenSet = true; m_developerProviderName.assign(value); }
    inline UpdateIdentityPoolRequest& WithDeveloperProviderName(const Aws::String& value) { SetDeveloperProviderName(value); return *this;}
    inline UpdateIdentityPoolRequest& WithDeveloperProviderName(Aws::String&& value) { SetDeveloperProviderName(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& WithDeveloperProviderName(const char* value) { SetDeveloperProviderName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARNs of the OpenID Connect providers.</p>
     */
    inline const Aws::Vector<Aws::String>& GetOpenIdConnectProviderARNs() const{ return m_openIdConnectProviderARNs; }
    inline bool OpenIdConnectProviderARNsHasBeenSet() const { return m_openIdConnectProviderARNsHasBeenSet; }
    inline void SetOpenIdConnectProviderARNs(const Aws::Vector<Aws::String>& value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs = value; }
    inline void SetOpenIdConnectProviderARNs(Aws::Vector<Aws::String>&& value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs = std::move(value); }
    inline UpdateIdentityPoolRequest& WithOpenIdConnectProviderARNs(const Aws::Vector<Aws::String>& value) { SetOpenIdConnectProviderARNs(value); return *this;}
    inline UpdateIdentityPoolRequest& WithOpenIdConnectProviderARNs(Aws::Vector<Aws::String>&& value) { SetOpenIdConnectProviderARNs(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& AddOpenIdConnectProviderARNs(const Aws::String& value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs.push_back(value); return *this; }
    inline UpdateIdentityPoolRequest& AddOpenIdConnectProviderARNs(Aws::String&& value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs.push_back(std::move(value)); return *this; }
    inline UpdateIdentityPoolRequest& AddOpenIdConnectProviderARNs(const char* value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>A list representing an Amazon Cognito user pool and its client ID.</p>
     */
    inline const Aws::Vector<CognitoIdentityProvider>& GetCognitoIdentityProviders() const{ return m_cognitoIdentityProviders; }
    inline bool CognitoIdentityProvidersHasBeenSet() const { return m_cognitoIdentityProvidersHasBeenSet; }
    inline void SetCognitoIdentityProviders(const Aws::Vector<CognitoIdentityProvider>& value) { m_cognitoIdentityProvidersHasBeenSet = true; m_cognitoIdentityProviders = value; }
    inline void SetCognitoIdentityProviders(Aws::Vector<CognitoIdentityProvider>&& value) { m_cognitoIdentityProvidersHasBeenSet = true; m_cognitoIdentityProviders = std::move(value); }
    inline UpdateIdentityPoolRequest& WithCognitoIdentityProviders(const Aws::Vector<CognitoIdentityProvider>& value) { SetCognitoIdentityProviders(value); return *this;}
    inline UpdateIdentityPoolRequest& WithCognitoIdentityProviders(Aws::Vector<CognitoIdentityProvider>&& value) { SetCognitoIdentityProviders(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& AddCognitoIdentityProviders(const CognitoIdentityProvider& value) { m_cognitoIdentityProvidersHasBeenSet = true; m_cognitoIdentityProviders.push_back(value); return *this; }
    inline UpdateIdentityPoolRequest& AddCognitoIdentityProviders(CognitoIdentityProvider&& value) { m_cognitoIdentityProvidersHasBeenSet = true; m_cognitoIdentityProviders.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>An array of Amazon Resource Names (ARNs) of the SAML provider for your
     * identity pool.</p>
     */
    inline const Aws::Vector<Aws::String>& GetSamlProviderARNs() const{ return m_samlProviderARNs; }
    inline bool SamlProviderARNsHasBeenSet() const { return m_samlProviderARNsHasBeenSet; }
    inline void SetSamlProviderARNs(const Aws::Vector<Aws::String>& value) { m_samlProviderARNsHasBeenSet = true; m_samlProviderARNs = value; }
    inline void SetSamlProviderARNs(Aws::Vector<Aws::String>&& value) { m_samlProviderARNsHasBeenSet = true; m_samlProviderARNs = std::move(value); }
    inline UpdateIdentityPoolRequest& WithSamlProviderARNs(const Aws::Vector<Aws::String>& value) { SetSamlProviderARNs(value); return *this;}
    inline UpdateIdentityPoolRequest& WithSamlProviderARNs(Aws::Vector<Aws::String>&& value) { SetSamlProviderARNs(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& AddSamlProviderARNs(const Aws::String& value) { m_samlProviderARNsHasBeenSet = true; m_samlProviderARNs.push_back(value); return *this; }
    inline UpdateIdentityPoolRequest& AddSamlProviderARNs(Aws::String&& value) { m_samlProviderARNsHasBeenSet = true; m_samlProviderARNs.push_back(std::move(value)); return *this; }
    inline UpdateIdentityPoolRequest& AddSamlProviderARNs(const char* value) { m_samlProviderARNsHasBeenSet = true; m_samlProviderARNs.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The tags that are assigned to the identity pool. A tag is a label that you
     * can apply to identity pools to categorize and manage them in different ways,
     * such as by purpose, owner, environment, or other criteria.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetIdentityPoolTags() const{ return m_identityPoolTags; }
    inline bool IdentityPoolTagsHasBeenSet() const { return m_identityPoolTagsHasBeenSet; }
    inline void SetIdentityPoolTags(const Aws::Map<Aws::String, Aws::String>& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags = value; }
    inline void SetIdentityPoolTags(Aws::Map<Aws::String, Aws::String>&& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags = std::move(value); }
    inline UpdateIdentityPoolRequest& WithIdentityPoolTags(const Aws::Map<Aws::String, Aws::String>& value) { SetIdentityPoolTags(value); return *this;}
    inline UpdateIdentityPoolRequest& WithIdentityPoolTags(Aws::Map<Aws::String, Aws::String>&& value) { SetIdentityPoolTags(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& AddIdentityPoolTags(const Aws::String& key, const Aws::String& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(key, value); return *this; }
    inline UpdateIdentityPoolRequest& AddIdentityPoolTags(Aws::String&& key, const Aws::String& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(std::move(key), value); return *this; }
    inline UpdateIdentityPoolRequest& AddIdentityPoolTags(const Aws::String& key, Aws::String&& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(key, std::move(value)); return *this; }
    inline UpdateIdentityPoolRequest& AddIdentityPoolTags(Aws::String&& key, Aws::String&& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(std::move(key), std::move(value)); return *this; }
    inline UpdateIdentityPoolRequest& AddIdentityPoolTags(const char* key, Aws::String&& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(key, std::move(value)); return *this; }
    inline UpdateIdentityPoolRequest& AddIdentityPoolTags(Aws::String&& key, const char* value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(std::move(key), value); return *this; }
    inline UpdateIdentityPoolRequest& AddIdentityPoolTags(const char* key, const char* value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(key, value); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline bool RequestIdHasBeenSet() const { return m_requestIdHasBeenSet; }
    inline void SetRequestId(const Aws::String& value) { m_requestIdHasBeenSet = true; m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestIdHasBeenSet = true; m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestIdHasBeenSet = true; m_requestId.assign(value); }
    inline UpdateIdentityPoolRequest& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline UpdateIdentityPoolRequest& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline UpdateIdentityPoolRequest& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_identityPoolId;
    bool m_identityPoolIdHasBeenSet = false;

    Aws::String m_identityPoolName;
    bool m_identityPoolNameHasBeenSet = false;

    bool m_allowUnauthenticatedIdentities;
    bool m_allowUnauthenticatedIdentitiesHasBeenSet = false;

    bool m_allowClassicFlow;
    bool m_allowClassicFlowHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_supportedLoginProviders;
    bool m_supportedLoginProvidersHasBeenSet = false;

    Aws::String m_developerProviderName;
    bool m_developerProviderNameHasBeenSet = false;

    Aws::Vector<Aws::String> m_openIdConnectProviderARNs;
    bool m_openIdConnectProviderARNsHasBeenSet = false;

    Aws::Vector<CognitoIdentityProvider> m_cognitoIdentityProviders;
    bool m_cognitoIdentityProvidersHasBeenSet = false;

    Aws::Vector<Aws::String> m_samlProviderARNs;
    bool m_samlProviderARNsHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_identityPoolTags;
    bool m_identityPoolTagsHasBeenSet = false;

    Aws::String m_requestId;
    bool m_requestIdHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
