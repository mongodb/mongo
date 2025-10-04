/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/cognito-identity/model/RoleMapping.h>
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
   * <p>Returned in response to a successful <code>GetIdentityPoolRoles</code>
   * operation.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/GetIdentityPoolRolesResponse">AWS
   * API Reference</a></p>
   */
  class GetIdentityPoolRolesResult
  {
  public:
    AWS_COGNITOIDENTITY_API GetIdentityPoolRolesResult();
    AWS_COGNITOIDENTITY_API GetIdentityPoolRolesResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_COGNITOIDENTITY_API GetIdentityPoolRolesResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>An identity pool ID in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityPoolId() const{ return m_identityPoolId; }
    inline void SetIdentityPoolId(const Aws::String& value) { m_identityPoolId = value; }
    inline void SetIdentityPoolId(Aws::String&& value) { m_identityPoolId = std::move(value); }
    inline void SetIdentityPoolId(const char* value) { m_identityPoolId.assign(value); }
    inline GetIdentityPoolRolesResult& WithIdentityPoolId(const Aws::String& value) { SetIdentityPoolId(value); return *this;}
    inline GetIdentityPoolRolesResult& WithIdentityPoolId(Aws::String&& value) { SetIdentityPoolId(std::move(value)); return *this;}
    inline GetIdentityPoolRolesResult& WithIdentityPoolId(const char* value) { SetIdentityPoolId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The map of roles associated with this pool. Currently only authenticated and
     * unauthenticated roles are supported.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetRoles() const{ return m_roles; }
    inline void SetRoles(const Aws::Map<Aws::String, Aws::String>& value) { m_roles = value; }
    inline void SetRoles(Aws::Map<Aws::String, Aws::String>&& value) { m_roles = std::move(value); }
    inline GetIdentityPoolRolesResult& WithRoles(const Aws::Map<Aws::String, Aws::String>& value) { SetRoles(value); return *this;}
    inline GetIdentityPoolRolesResult& WithRoles(Aws::Map<Aws::String, Aws::String>&& value) { SetRoles(std::move(value)); return *this;}
    inline GetIdentityPoolRolesResult& AddRoles(const Aws::String& key, const Aws::String& value) { m_roles.emplace(key, value); return *this; }
    inline GetIdentityPoolRolesResult& AddRoles(Aws::String&& key, const Aws::String& value) { m_roles.emplace(std::move(key), value); return *this; }
    inline GetIdentityPoolRolesResult& AddRoles(const Aws::String& key, Aws::String&& value) { m_roles.emplace(key, std::move(value)); return *this; }
    inline GetIdentityPoolRolesResult& AddRoles(Aws::String&& key, Aws::String&& value) { m_roles.emplace(std::move(key), std::move(value)); return *this; }
    inline GetIdentityPoolRolesResult& AddRoles(const char* key, Aws::String&& value) { m_roles.emplace(key, std::move(value)); return *this; }
    inline GetIdentityPoolRolesResult& AddRoles(Aws::String&& key, const char* value) { m_roles.emplace(std::move(key), value); return *this; }
    inline GetIdentityPoolRolesResult& AddRoles(const char* key, const char* value) { m_roles.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>How users for a specific identity provider are to mapped to roles. This is a
     * String-to-<a>RoleMapping</a> object map. The string identifies the identity
     * provider, for example, "graph.facebook.com" or
     * "cognito-idp.us-east-1.amazonaws.com/us-east-1_abcdefghi:app_client_id".</p>
     */
    inline const Aws::Map<Aws::String, RoleMapping>& GetRoleMappings() const{ return m_roleMappings; }
    inline void SetRoleMappings(const Aws::Map<Aws::String, RoleMapping>& value) { m_roleMappings = value; }
    inline void SetRoleMappings(Aws::Map<Aws::String, RoleMapping>&& value) { m_roleMappings = std::move(value); }
    inline GetIdentityPoolRolesResult& WithRoleMappings(const Aws::Map<Aws::String, RoleMapping>& value) { SetRoleMappings(value); return *this;}
    inline GetIdentityPoolRolesResult& WithRoleMappings(Aws::Map<Aws::String, RoleMapping>&& value) { SetRoleMappings(std::move(value)); return *this;}
    inline GetIdentityPoolRolesResult& AddRoleMappings(const Aws::String& key, const RoleMapping& value) { m_roleMappings.emplace(key, value); return *this; }
    inline GetIdentityPoolRolesResult& AddRoleMappings(Aws::String&& key, const RoleMapping& value) { m_roleMappings.emplace(std::move(key), value); return *this; }
    inline GetIdentityPoolRolesResult& AddRoleMappings(const Aws::String& key, RoleMapping&& value) { m_roleMappings.emplace(key, std::move(value)); return *this; }
    inline GetIdentityPoolRolesResult& AddRoleMappings(Aws::String&& key, RoleMapping&& value) { m_roleMappings.emplace(std::move(key), std::move(value)); return *this; }
    inline GetIdentityPoolRolesResult& AddRoleMappings(const char* key, RoleMapping&& value) { m_roleMappings.emplace(key, std::move(value)); return *this; }
    inline GetIdentityPoolRolesResult& AddRoleMappings(const char* key, const RoleMapping& value) { m_roleMappings.emplace(key, value); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetIdentityPoolRolesResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetIdentityPoolRolesResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetIdentityPoolRolesResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_identityPoolId;

    Aws::Map<Aws::String, Aws::String> m_roles;

    Aws::Map<Aws::String, RoleMapping> m_roleMappings;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
