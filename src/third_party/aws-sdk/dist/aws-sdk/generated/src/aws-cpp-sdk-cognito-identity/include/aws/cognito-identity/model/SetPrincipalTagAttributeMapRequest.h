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
   */
  class SetPrincipalTagAttributeMapRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API SetPrincipalTagAttributeMapRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "SetPrincipalTagAttributeMap"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>The ID of the Identity Pool you want to set attribute mappings for.</p>
     */
    inline const Aws::String& GetIdentityPoolId() const{ return m_identityPoolId; }
    inline bool IdentityPoolIdHasBeenSet() const { return m_identityPoolIdHasBeenSet; }
    inline void SetIdentityPoolId(const Aws::String& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = value; }
    inline void SetIdentityPoolId(Aws::String&& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = std::move(value); }
    inline void SetIdentityPoolId(const char* value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId.assign(value); }
    inline SetPrincipalTagAttributeMapRequest& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline SetPrincipalTagAttributeMapRequest& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline SetPrincipalTagAttributeMapRequest& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The provider name you want to use for attribute mappings.</p>
     */
    inline const Aws::String& GetIdentityProviderName() const{ return m_identityProviderName; }
    inline bool IdentityProviderNameHasBeenSet() const { return m_identityProviderNameHasBeenSet; }
    inline void SetIdentityProviderName(const Aws::String& value) { m_identityProviderNameHasBeenSet = true; m_identityProviderName = value; }
    inline void SetIdentityProviderName(Aws::String&& value) { m_identityProviderNameHasBeenSet = true; m_identityProviderName = std::move(value); }
    inline void SetIdentityProviderName(const char* value) { m_identityProviderNameHasBeenSet = true; m_identityProviderName.assign(value); }
    inline SetPrincipalTagAttributeMapRequest& WithIdentityProviderName(const Aws::String& value) { SetIdentityProviderName(value); return *this;}
    inline SetPrincipalTagAttributeMapRequest& WithIdentityProviderName(Aws::String&& value) { SetIdentityProviderName(std::move(value)); return *this;}
    inline SetPrincipalTagAttributeMapRequest& WithIdentityProviderName(const char* value) { SetIdentityProviderName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>You can use this operation to use default (username and clientID) attribute
     * mappings.</p>
     */
    inline bool GetUseDefaults() const{ return m_useDefaults; }
    inline bool UseDefaultsHasBeenSet() const { return m_useDefaultsHasBeenSet; }
    inline void SetUseDefaults(bool value) { m_useDefaultsHasBeenSet = true; m_useDefaults = value; }
    inline SetPrincipalTagAttributeMapRequest& WithUseDefaults(bool value) { SetUseDefaults(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>You can use this operation to add principal tags.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetPrincipalTags() const{ return m_principalTags; }
    inline bool PrincipalTagsHasBeenSet() const { return m_principalTagsHasBeenSet; }
    inline void SetPrincipalTags(const Aws::Map<Aws::String, Aws::String>& value) { m_principalTagsHasBeenSet = true; m_principalTags = value; }
    inline void SetPrincipalTags(Aws::Map<Aws::String, Aws::String>&& value) { m_principalTagsHasBeenSet = true; m_principalTags = std::move(value); }
    inline SetPrincipalTagAttributeMapRequest& WithPrincipalTags(const Aws::Map<Aws::String, Aws::String>& value) { SetPrincipalTags(value); return *this;}
    inline SetPrincipalTagAttributeMapRequest& WithPrincipalTags(Aws::Map<Aws::String, Aws::String>&& value) { SetPrincipalTags(std::move(value)); return *this;}
    inline SetPrincipalTagAttributeMapRequest& AddPrincipalTags(const Aws::String& key, const Aws::String& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(key, value); return *this; }
    inline SetPrincipalTagAttributeMapRequest& AddPrincipalTags(Aws::String&& key, const Aws::String& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(std::move(key), value); return *this; }
    inline SetPrincipalTagAttributeMapRequest& AddPrincipalTags(const Aws::String& key, Aws::String&& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(key, std::move(value)); return *this; }
    inline SetPrincipalTagAttributeMapRequest& AddPrincipalTags(Aws::String&& key, Aws::String&& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(std::move(key), std::move(value)); return *this; }
    inline SetPrincipalTagAttributeMapRequest& AddPrincipalTags(const char* key, Aws::String&& value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(key, std::move(value)); return *this; }
    inline SetPrincipalTagAttributeMapRequest& AddPrincipalTags(Aws::String&& key, const char* value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(std::move(key), value); return *this; }
    inline SetPrincipalTagAttributeMapRequest& AddPrincipalTags(const char* key, const char* value) { m_principalTagsHasBeenSet = true; m_principalTags.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_identityPoolId;
    bool m_identityPoolIdHasBeenSet = false;

    Aws::String m_identityProviderName;
    bool m_identityProviderNameHasBeenSet = false;

    bool m_useDefaults;
    bool m_useDefaultsHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_principalTags;
    bool m_principalTagsHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
