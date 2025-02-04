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
   * <p>Input to the CreateIdentityPool action.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/CreateIdentityPoolInput">AWS
   * API Reference</a></p>
   */
  class CreateIdentityPoolRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API CreateIdentityPoolRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateIdentityPool"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>A string that you provide.</p>
     */
    inline const Aws::String& GetIdentityPoolName() const{ return m_identityPoolName; }
    inline bool IdentityPoolNameHasBeenSet() const { return m_identityPoolNameHasBeenSet; }
    inline void SetIdentityPoolName(const Aws::String& value) { m_identityPoolNameHasBeenSet = true; m_identityPoolName = value; }
    inline void SetIdentityPoolName(Aws::String&& value) { m_identityPoolNameHasBeenSet = true; m_identityPoolName = std::move(value); }
    inline void SetIdentityPoolName(const char* value) { m_identityPoolNameHasBeenSet = true; m_identityPoolName.assign(value); }
    inline CreateIdentityPoolRequest& WithIdentityPoolName(const Aws::String& value) { SetIdentityPoolName(value); return *this;}
    inline CreateIdentityPoolRequest& WithIdentityPoolName(Aws::String&& value) { SetIdentityPoolName(std::move(value)); return *this;}
    inline CreateIdentityPoolRequest& WithIdentityPoolName(const char* value) { SetIdentityPoolName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>TRUE if the identity pool supports unauthenticated logins.</p>
     */
    inline bool GetAllowUnauthenticatedIdentities() const{ return m_allowUnauthenticatedIdentities; }
    inline bool AllowUnauthenticatedIdentitiesHasBeenSet() const { return m_allowUnauthenticatedIdentitiesHasBeenSet; }
    inline void SetAllowUnauthenticatedIdentities(bool value) { m_allowUnauthenticatedIdentitiesHasBeenSet = true; m_allowUnauthenticatedIdentities = value; }
    inline CreateIdentityPoolRequest& WithAllowUnauthenticatedIdentities(bool value) { SetAllowUnauthenticatedIdentities(value); return *this;}
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
    inline CreateIdentityPoolRequest& WithAllowClassicFlow(bool value) { SetAllowClassicFlow(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Optional key:value pairs mapping provider names to provider app IDs.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetSupportedLoginProviders() const{ return m_supportedLoginProviders; }
    inline bool SupportedLoginProvidersHasBeenSet() const { return m_supportedLoginProvidersHasBeenSet; }
    inline void SetSupportedLoginProviders(const Aws::Map<Aws::String, Aws::String>& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders = value; }
    inline void SetSupportedLoginProviders(Aws::Map<Aws::String, Aws::String>&& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders = std::move(value); }
    inline CreateIdentityPoolRequest& WithSupportedLoginProviders(const Aws::Map<Aws::String, Aws::String>& value) { SetSupportedLoginProviders(value); return *this;}
    inline CreateIdentityPoolRequest& WithSupportedLoginProviders(Aws::Map<Aws::String, Aws::String>&& value) { SetSupportedLoginProviders(std::move(value)); return *this;}
    inline CreateIdentityPoolRequest& AddSupportedLoginProviders(const Aws::String& key, const Aws::String& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(key, value); return *this; }
    inline CreateIdentityPoolRequest& AddSupportedLoginProviders(Aws::String&& key, const Aws::String& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(std::move(key), value); return *this; }
    inline CreateIdentityPoolRequest& AddSupportedLoginProviders(const Aws::String& key, Aws::String&& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(key, std::move(value)); return *this; }
    inline CreateIdentityPoolRequest& AddSupportedLoginProviders(Aws::String&& key, Aws::String&& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(std::move(key), std::move(value)); return *this; }
    inline CreateIdentityPoolRequest& AddSupportedLoginProviders(const char* key, Aws::String&& value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(key, std::move(value)); return *this; }
    inline CreateIdentityPoolRequest& AddSupportedLoginProviders(Aws::String&& key, const char* value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(std::move(key), value); return *this; }
    inline CreateIdentityPoolRequest& AddSupportedLoginProviders(const char* key, const char* value) { m_supportedLoginProvidersHasBeenSet = true; m_supportedLoginProviders.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The "domain" by which Cognito will refer to your users. This name acts as a
     * placeholder that allows your backend and the Cognito service to communicate
     * about the developer provider. For the <code>DeveloperProviderName</code>, you
     * can use letters as well as period (<code>.</code>), underscore (<code>_</code>),
     * and dash (<code>-</code>).</p> <p>Once you have set a developer provider name,
     * you cannot change it. Please take care in setting this parameter.</p>
     */
    inline const Aws::String& GetDeveloperProviderName() const{ return m_developerProviderName; }
    inline bool DeveloperProviderNameHasBeenSet() const { return m_developerProviderNameHasBeenSet; }
    inline void SetDeveloperProviderName(const Aws::String& value) { m_developerProviderNameHasBeenSet = true; m_developerProviderName = value; }
    inline void SetDeveloperProviderName(Aws::String&& value) { m_developerProviderNameHasBeenSet = true; m_developerProviderName = std::move(value); }
    inline void SetDeveloperProviderName(const char* value) { m_developerProviderNameHasBeenSet = true; m_developerProviderName.assign(value); }
    inline CreateIdentityPoolRequest& WithDeveloperProviderName(const Aws::String& value) { SetDeveloperProviderName(value); return *this;}
    inline CreateIdentityPoolRequest& WithDeveloperProviderName(Aws::String&& value) { SetDeveloperProviderName(std::move(value)); return *this;}
    inline CreateIdentityPoolRequest& WithDeveloperProviderName(const char* value) { SetDeveloperProviderName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Names (ARN) of the OpenID Connect providers.</p>
     */
    inline const Aws::Vector<Aws::String>& GetOpenIdConnectProviderARNs() const{ return m_openIdConnectProviderARNs; }
    inline bool OpenIdConnectProviderARNsHasBeenSet() const { return m_openIdConnectProviderARNsHasBeenSet; }
    inline void SetOpenIdConnectProviderARNs(const Aws::Vector<Aws::String>& value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs = value; }
    inline void SetOpenIdConnectProviderARNs(Aws::Vector<Aws::String>&& value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs = std::move(value); }
    inline CreateIdentityPoolRequest& WithOpenIdConnectProviderARNs(const Aws::Vector<Aws::String>& value) { SetOpenIdConnectProviderARNs(value); return *this;}
    inline CreateIdentityPoolRequest& WithOpenIdConnectProviderARNs(Aws::Vector<Aws::String>&& value) { SetOpenIdConnectProviderARNs(std::move(value)); return *this;}
    inline CreateIdentityPoolRequest& AddOpenIdConnectProviderARNs(const Aws::String& value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs.push_back(value); return *this; }
    inline CreateIdentityPoolRequest& AddOpenIdConnectProviderARNs(Aws::String&& value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs.push_back(std::move(value)); return *this; }
    inline CreateIdentityPoolRequest& AddOpenIdConnectProviderARNs(const char* value) { m_openIdConnectProviderARNsHasBeenSet = true; m_openIdConnectProviderARNs.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>An array of Amazon Cognito user pools and their client IDs.</p>
     */
    inline const Aws::Vector<CognitoIdentityProvider>& GetCognitoIdentityProviders() const{ return m_cognitoIdentityProviders; }
    inline bool CognitoIdentityProvidersHasBeenSet() const { return m_cognitoIdentityProvidersHasBeenSet; }
    inline void SetCognitoIdentityProviders(const Aws::Vector<CognitoIdentityProvider>& value) { m_cognitoIdentityProvidersHasBeenSet = true; m_cognitoIdentityProviders = value; }
    inline void SetCognitoIdentityProviders(Aws::Vector<CognitoIdentityProvider>&& value) { m_cognitoIdentityProvidersHasBeenSet = true; m_cognitoIdentityProviders = std::move(value); }
    inline CreateIdentityPoolRequest& WithCognitoIdentityProviders(const Aws::Vector<CognitoIdentityProvider>& value) { SetCognitoIdentityProviders(value); return *this;}
    inline CreateIdentityPoolRequest& WithCognitoIdentityProviders(Aws::Vector<CognitoIdentityProvider>&& value) { SetCognitoIdentityProviders(std::move(value)); return *this;}
    inline CreateIdentityPoolRequest& AddCognitoIdentityProviders(const CognitoIdentityProvider& value) { m_cognitoIdentityProvidersHasBeenSet = true; m_cognitoIdentityProviders.push_back(value); return *this; }
    inline CreateIdentityPoolRequest& AddCognitoIdentityProviders(CognitoIdentityProvider&& value) { m_cognitoIdentityProvidersHasBeenSet = true; m_cognitoIdentityProviders.push_back(std::move(value)); return *this; }
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
    inline CreateIdentityPoolRequest& WithSamlProviderARNs(const Aws::Vector<Aws::String>& value) { SetSamlProviderARNs(value); return *this;}
    inline CreateIdentityPoolRequest& WithSamlProviderARNs(Aws::Vector<Aws::String>&& value) { SetSamlProviderARNs(std::move(value)); return *this;}
    inline CreateIdentityPoolRequest& AddSamlProviderARNs(const Aws::String& value) { m_samlProviderARNsHasBeenSet = true; m_samlProviderARNs.push_back(value); return *this; }
    inline CreateIdentityPoolRequest& AddSamlProviderARNs(Aws::String&& value) { m_samlProviderARNsHasBeenSet = true; m_samlProviderARNs.push_back(std::move(value)); return *this; }
    inline CreateIdentityPoolRequest& AddSamlProviderARNs(const char* value) { m_samlProviderARNsHasBeenSet = true; m_samlProviderARNs.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>Tags to assign to the identity pool. A tag is a label that you can apply to
     * identity pools to categorize and manage them in different ways, such as by
     * purpose, owner, environment, or other criteria.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetIdentityPoolTags() const{ return m_identityPoolTags; }
    inline bool IdentityPoolTagsHasBeenSet() const { return m_identityPoolTagsHasBeenSet; }
    inline void SetIdentityPoolTags(const Aws::Map<Aws::String, Aws::String>& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags = value; }
    inline void SetIdentityPoolTags(Aws::Map<Aws::String, Aws::String>&& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags = std::move(value); }
    inline CreateIdentityPoolRequest& WithIdentityPoolTags(const Aws::Map<Aws::String, Aws::String>& value) { SetIdentityPoolTags(value); return *this;}
    inline CreateIdentityPoolRequest& WithIdentityPoolTags(Aws::Map<Aws::String, Aws::String>&& value) { SetIdentityPoolTags(std::move(value)); return *this;}
    inline CreateIdentityPoolRequest& AddIdentityPoolTags(const Aws::String& key, const Aws::String& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(key, value); return *this; }
    inline CreateIdentityPoolRequest& AddIdentityPoolTags(Aws::String&& key, const Aws::String& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(std::move(key), value); return *this; }
    inline CreateIdentityPoolRequest& AddIdentityPoolTags(const Aws::String& key, Aws::String&& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(key, std::move(value)); return *this; }
    inline CreateIdentityPoolRequest& AddIdentityPoolTags(Aws::String&& key, Aws::String&& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(std::move(key), std::move(value)); return *this; }
    inline CreateIdentityPoolRequest& AddIdentityPoolTags(const char* key, Aws::String&& value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(key, std::move(value)); return *this; }
    inline CreateIdentityPoolRequest& AddIdentityPoolTags(Aws::String&& key, const char* value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(std::move(key), value); return *this; }
    inline CreateIdentityPoolRequest& AddIdentityPoolTags(const char* key, const char* value) { m_identityPoolTagsHasBeenSet = true; m_identityPoolTags.emplace(key, value); return *this; }
    ///@}
  private:

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
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
