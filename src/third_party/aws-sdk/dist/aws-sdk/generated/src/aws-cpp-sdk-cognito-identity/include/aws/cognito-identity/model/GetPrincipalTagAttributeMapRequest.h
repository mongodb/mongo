/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/cognito-identity/CognitoIdentityRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

  /**
   */
  class GetPrincipalTagAttributeMapRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API GetPrincipalTagAttributeMapRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetPrincipalTagAttributeMap"; }

    AWS_COGNITOIDENTITY_API Aws::String SerializePayload() const override;

    AWS_COGNITOIDENTITY_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;


    ///@{
    /**
     * <p>You can use this operation to get the ID of the Identity Pool you setup
     * attribute mappings for.</p>
     */
    inline const Aws::String& GetIdentityPoolId() const{ return m_identityPoolId; }
    inline bool IdentityPoolIdHasBeenSet() const { return m_identityPoolIdHasBeenSet; }
    inline void SetIdentityPoolId(const Aws::String& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = value; }
    inline void SetIdentityPoolId(Aws::String&& value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId = std::move(value); }
    inline void SetIdentityPoolId(const char* value) { m_identityPoolIdHasBeenSet = true; m_identityPoolId.assign(value); }
    inline GetPrincipalTagAttributeMapRequest& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline GetPrincipalTagAttributeMapRequest& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline GetPrincipalTagAttributeMapRequest& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>You can use this operation to get the provider name.</p>
     */
    inline const Aws::String& GetIdentityProviderName() const{ return m_identityProviderName; }
    inline bool IdentityProviderNameHasBeenSet() const { return m_identityProviderNameHasBeenSet; }
    inline void SetIdentityProviderName(const Aws::String& value) { m_identityProviderNameHasBeenSet = true; m_identityProviderName = value; }
    inline void SetIdentityProviderName(Aws::String&& value) { m_identityProviderNameHasBeenSet = true; m_identityProviderName = std::move(value); }
    inline void SetIdentityProviderName(const char* value) { m_identityProviderNameHasBeenSet = true; m_identityProviderName.assign(value); }
    inline GetPrincipalTagAttributeMapRequest& WithIdentityProviderName(const Aws::String& value) { SetIdentityProviderName(value); return *this;}
    inline GetPrincipalTagAttributeMapRequest& WithIdentityProviderName(Aws::String&& value) { SetIdentityProviderName(std::move(value)); return *this;}
    inline GetPrincipalTagAttributeMapRequest& WithIdentityProviderName(const char* value) { SetIdentityProviderName(value); return *this;}
    ///@}
  private:

    Aws::String m_identityPoolId;
    bool m_identityPoolIdHasBeenSet = false;

    Aws::String m_identityProviderName;
    bool m_identityProviderNameHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
