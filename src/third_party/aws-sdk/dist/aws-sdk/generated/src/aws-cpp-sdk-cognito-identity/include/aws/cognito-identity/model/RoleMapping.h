/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/cognito-identity/model/RoleMappingType.h>
#include <aws/cognito-identity/model/AmbiguousRoleResolutionType.h>
#include <aws/cognito-identity/model/RulesConfigurationType.h>
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
   * <p>A role mapping.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/RoleMapping">AWS
   * API Reference</a></p>
   */
  class RoleMapping
  {
  public:
    AWS_COGNITOIDENTITY_API RoleMapping();
    AWS_COGNITOIDENTITY_API RoleMapping(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API RoleMapping& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The role mapping type. Token will use <code>cognito:roles</code> and
     * <code>cognito:preferred_role</code> claims from the Cognito identity provider
     * token to map groups to roles. Rules will attempt to match claims from the token
     * to map to a role.</p>
     */
    inline const RoleMappingType& GetType() const{ return m_type; }
    inline bool TypeHasBeenSet() const { return m_typeHasBeenSet; }
    inline void SetType(const RoleMappingType& value) { m_typeHasBeenSet = true; m_type = value; }
    inline void SetType(RoleMappingType&& value) { m_typeHasBeenSet = true; m_type = std::move(value); }
    inline RoleMapping& WithType(const RoleMappingType& value) { SetType(value); return *this;}
    inline RoleMapping& WithType(RoleMappingType&& value) { SetType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If you specify Token or Rules as the <code>Type</code>,
     * <code>AmbiguousRoleResolution</code> is required.</p> <p>Specifies the action to
     * be taken if either no rules match the claim value for the <code>Rules</code>
     * type, or there is no <code>cognito:preferred_role</code> claim and there are
     * multiple <code>cognito:roles</code> matches for the <code>Token</code> type.</p>
     */
    inline const AmbiguousRoleResolutionType& GetAmbiguousRoleResolution() const{ return m_ambiguousRoleResolution; }
    inline bool AmbiguousRoleResolutionHasBeenSet() const { return m_ambiguousRoleResolutionHasBeenSet; }
    inline void SetAmbiguousRoleResolution(const AmbiguousRoleResolutionType& value) { m_ambiguousRoleResolutionHasBeenSet = true; m_ambiguousRoleResolution = value; }
    inline void SetAmbiguousRoleResolution(AmbiguousRoleResolutionType&& value) { m_ambiguousRoleResolutionHasBeenSet = true; m_ambiguousRoleResolution = std::move(value); }
    inline RoleMapping& WithAmbiguousRoleResolution(const AmbiguousRoleResolutionType& value) { SetAmbiguousRoleResolution(value); return *this;}
    inline RoleMapping& WithAmbiguousRoleResolution(AmbiguousRoleResolutionType&& value) { SetAmbiguousRoleResolution(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The rules to be used for mapping users to roles.</p> <p>If you specify Rules
     * as the role mapping type, <code>RulesConfiguration</code> is required.</p>
     */
    inline const RulesConfigurationType& GetRulesConfiguration() const{ return m_rulesConfiguration; }
    inline bool RulesConfigurationHasBeenSet() const { return m_rulesConfigurationHasBeenSet; }
    inline void SetRulesConfiguration(const RulesConfigurationType& value) { m_rulesConfigurationHasBeenSet = true; m_rulesConfiguration = value; }
    inline void SetRulesConfiguration(RulesConfigurationType&& value) { m_rulesConfigurationHasBeenSet = true; m_rulesConfiguration = std::move(value); }
    inline RoleMapping& WithRulesConfiguration(const RulesConfigurationType& value) { SetRulesConfiguration(value); return *this;}
    inline RoleMapping& WithRulesConfiguration(RulesConfigurationType&& value) { SetRulesConfiguration(std::move(value)); return *this;}
    ///@}
  private:

    RoleMappingType m_type;
    bool m_typeHasBeenSet = false;

    AmbiguousRoleResolutionType m_ambiguousRoleResolution;
    bool m_ambiguousRoleResolutionHasBeenSet = false;

    RulesConfigurationType m_rulesConfiguration;
    bool m_rulesConfigurationHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
