/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/RoleMapping.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{

RoleMapping::RoleMapping() : 
    m_type(RoleMappingType::NOT_SET),
    m_typeHasBeenSet(false),
    m_ambiguousRoleResolution(AmbiguousRoleResolutionType::NOT_SET),
    m_ambiguousRoleResolutionHasBeenSet(false),
    m_rulesConfigurationHasBeenSet(false)
{
}

RoleMapping::RoleMapping(JsonView jsonValue)
  : RoleMapping()
{
  *this = jsonValue;
}

RoleMapping& RoleMapping::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Type"))
  {
    m_type = RoleMappingTypeMapper::GetRoleMappingTypeForName(jsonValue.GetString("Type"));

    m_typeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("AmbiguousRoleResolution"))
  {
    m_ambiguousRoleResolution = AmbiguousRoleResolutionTypeMapper::GetAmbiguousRoleResolutionTypeForName(jsonValue.GetString("AmbiguousRoleResolution"));

    m_ambiguousRoleResolutionHasBeenSet = true;
  }

  if(jsonValue.ValueExists("RulesConfiguration"))
  {
    m_rulesConfiguration = jsonValue.GetObject("RulesConfiguration");

    m_rulesConfigurationHasBeenSet = true;
  }

  return *this;
}

JsonValue RoleMapping::Jsonize() const
{
  JsonValue payload;

  if(m_typeHasBeenSet)
  {
   payload.WithString("Type", RoleMappingTypeMapper::GetNameForRoleMappingType(m_type));
  }

  if(m_ambiguousRoleResolutionHasBeenSet)
  {
   payload.WithString("AmbiguousRoleResolution", AmbiguousRoleResolutionTypeMapper::GetNameForAmbiguousRoleResolutionType(m_ambiguousRoleResolution));
  }

  if(m_rulesConfigurationHasBeenSet)
  {
   payload.WithObject("RulesConfiguration", m_rulesConfiguration.Jsonize());

  }

  return payload;
}

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
