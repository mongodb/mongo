/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/cognito-identity/CognitoIdentityRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/cognito-identity/model/RoleMapping.h>
#include <utility>

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

  /**
   * <p>Input to the <code>SetIdentityPoolRoles</code> action.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/SetIdentityPoolRolesInput">AWS
   * API Reference</a></p>
   */
  class SetIdentityPoolRolesRequest : public CognitoIdentityRequest
  {
  public:
    AWS_COGNITOIDENTITY_API SetIdentityPoolRolesRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "SetIdentityPoolRoles"; }

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
    inline SetIdentityPoolRolesRequest& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline SetIdentityPoolRolesRequest& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline SetIdentityPoolRolesRequest& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The map of roles associated with this pool. For a given role, the key will be
     * either "authenticated" or "unauthenticated" and the value will be the Role
     * ARN.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetRoles() const{ return m_roles; }
    inline bool RolesHasBeenSet() const { return m_rolesHasBeenSet; }
    inline void SetRoles(const Aws::Map<Aws::String, Aws::String>& value) { m_rolesHasBeenSet = true; m_roles = value; }
    inline void SetRoles(Aws::Map<Aws::String, Aws::String>&& value) { m_rolesHasBeenSet = true; m_roles = std::move(value); }
    inline SetIdentityPoolRolesRequest& WithRoles(const Aws::Map<Aws::String, Aws::String>& value) { SetRoles(value); return *this;}
    inline SetIdentityPoolRolesRequest& WithRoles(Aws::Map<Aws::String, Aws::String>&& value) { SetRoles(std::move(value)); return *this;}
    inline SetIdentityPoolRolesRequest& AddRoles(const Aws::String& key, const Aws::String& value) { m_rolesHasBeenSet = true; m_roles.emplace(key, value); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoles(Aws::String&& key, const Aws::String& value) { m_rolesHasBeenSet = true; m_roles.emplace(std::move(key), value); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoles(const Aws::String& key, Aws::String&& value) { m_rolesHasBeenSet = true; m_roles.emplace(key, std::move(value)); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoles(Aws::String&& key, Aws::String&& value) { m_rolesHasBeenSet = true; m_roles.emplace(std::move(key), std::move(value)); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoles(const char* key, Aws::String&& value) { m_rolesHasBeenSet = true; m_roles.emplace(key, std::move(value)); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoles(Aws::String&& key, const char* value) { m_rolesHasBeenSet = true; m_roles.emplace(std::move(key), value); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoles(const char* key, const char* value) { m_rolesHasBeenSet = true; m_roles.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>How users for a specific identity provider are to mapped to roles. This is a
     * string to <a>RoleMapping</a> object map. The string identifies the identity
     * provider, for example, "graph.facebook.com" or
     * "cognito-idp.us-east-1.amazonaws.com/us-east-1_abcdefghi:app_client_id".</p>
     * <p>Up to 25 rules can be specified per identity provider.</p>
     */
    inline const Aws::Map<Aws::String, RoleMapping>& GetRoleMappings() const{ return m_roleMappings; }
    inline bool RoleMappingsHasBeenSet() const { return m_roleMappingsHasBeenSet; }
    inline void SetRoleMappings(const Aws::Map<Aws::String, RoleMapping>& value) { m_roleMappingsHasBeenSet = true; m_roleMappings = value; }
    inline void SetRoleMappings(Aws::Map<Aws::String, RoleMapping>&& value) { m_roleMappingsHasBeenSet = true; m_roleMappings = std::move(value); }
    inline SetIdentityPoolRolesRequest& WithRoleMappings(const Aws::Map<Aws::String, RoleMapping>& value) { SetRoleMappings(value); return *this;}
    inline SetIdentityPoolRolesRequest& WithRoleMappings(Aws::Map<Aws::String, RoleMapping>&& value) { SetRoleMappings(std::move(value)); return *this;}
    inline SetIdentityPoolRolesRequest& AddRoleMappings(const Aws::String& key, const RoleMapping& value) { m_roleMappingsHasBeenSet = true; m_roleMappings.emplace(key, value); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoleMappings(Aws::String&& key, const RoleMapping& value) { m_roleMappingsHasBeenSet = true; m_roleMappings.emplace(std::move(key), value); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoleMappings(const Aws::String& key, RoleMapping&& value) { m_roleMappingsHasBeenSet = true; m_roleMappings.emplace(key, std::move(value)); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoleMappings(Aws::String&& key, RoleMapping&& value) { m_roleMappingsHasBeenSet = true; m_roleMappings.emplace(std::move(key), std::move(value)); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoleMappings(const char* key, RoleMapping&& value) { m_roleMappingsHasBeenSet = true; m_roleMappings.emplace(key, std::move(value)); return *this; }
    inline SetIdentityPoolRolesRequest& AddRoleMappings(const char* key, const RoleMapping& value) { m_roleMappingsHasBeenSet = true; m_roleMappings.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_identityPoolId;
    bool m_identityPoolIdHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_roles;
    bool m_rolesHasBeenSet = false;

    Aws::Map<Aws::String, RoleMapping> m_roleMappings;
    bool m_roleMappingsHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
