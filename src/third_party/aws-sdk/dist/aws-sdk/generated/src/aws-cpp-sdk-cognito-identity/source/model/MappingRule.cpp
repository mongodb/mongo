/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/MappingRule.h>
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

MappingRule::MappingRule() : 
    m_claimHasBeenSet(false),
    m_matchType(MappingRuleMatchType::NOT_SET),
    m_matchTypeHasBeenSet(false),
    m_valueHasBeenSet(false),
    m_roleARNHasBeenSet(false)
{
}

MappingRule::MappingRule(JsonView jsonValue)
  : MappingRule()
{
  *this = jsonValue;
}

MappingRule& MappingRule::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Claim"))
  {
    m_claim = jsonValue.GetString("Claim");

    m_claimHasBeenSet = true;
  }

  if(jsonValue.ValueExists("MatchType"))
  {
    m_matchType = MappingRuleMatchTypeMapper::GetMappingRuleMatchTypeForName(jsonValue.GetString("MatchType"));

    m_matchTypeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Value"))
  {
    m_value = jsonValue.GetString("Value");

    m_valueHasBeenSet = true;
  }

  if(jsonValue.ValueExists("RoleARN"))
  {
    m_roleARN = jsonValue.GetString("RoleARN");

    m_roleARNHasBeenSet = true;
  }

  return *this;
}

JsonValue MappingRule::Jsonize() const
{
  JsonValue payload;

  if(m_claimHasBeenSet)
  {
   payload.WithString("Claim", m_claim);

  }

  if(m_matchTypeHasBeenSet)
  {
   payload.WithString("MatchType", MappingRuleMatchTypeMapper::GetNameForMappingRuleMatchType(m_matchType));
  }

  if(m_valueHasBeenSet)
  {
   payload.WithString("Value", m_value);

  }

  if(m_roleARNHasBeenSet)
  {
   payload.WithString("RoleARN", m_roleARN);

  }

  return payload;
}

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
