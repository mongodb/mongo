/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/CodeSigningPolicies.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

CodeSigningPolicies::CodeSigningPolicies() : 
    m_untrustedArtifactOnDeployment(CodeSigningPolicy::NOT_SET),
    m_untrustedArtifactOnDeploymentHasBeenSet(false)
{
}

CodeSigningPolicies::CodeSigningPolicies(JsonView jsonValue)
  : CodeSigningPolicies()
{
  *this = jsonValue;
}

CodeSigningPolicies& CodeSigningPolicies::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("UntrustedArtifactOnDeployment"))
  {
    m_untrustedArtifactOnDeployment = CodeSigningPolicyMapper::GetCodeSigningPolicyForName(jsonValue.GetString("UntrustedArtifactOnDeployment"));

    m_untrustedArtifactOnDeploymentHasBeenSet = true;
  }

  return *this;
}

JsonValue CodeSigningPolicies::Jsonize() const
{
  JsonValue payload;

  if(m_untrustedArtifactOnDeploymentHasBeenSet)
  {
   payload.WithString("UntrustedArtifactOnDeployment", CodeSigningPolicyMapper::GetNameForCodeSigningPolicy(m_untrustedArtifactOnDeployment));
  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
