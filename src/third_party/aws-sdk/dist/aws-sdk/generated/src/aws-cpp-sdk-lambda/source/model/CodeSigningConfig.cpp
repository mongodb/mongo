/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/CodeSigningConfig.h>
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

CodeSigningConfig::CodeSigningConfig() : 
    m_codeSigningConfigIdHasBeenSet(false),
    m_codeSigningConfigArnHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_allowedPublishersHasBeenSet(false),
    m_codeSigningPoliciesHasBeenSet(false),
    m_lastModifiedHasBeenSet(false)
{
}

CodeSigningConfig::CodeSigningConfig(JsonView jsonValue)
  : CodeSigningConfig()
{
  *this = jsonValue;
}

CodeSigningConfig& CodeSigningConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("CodeSigningConfigId"))
  {
    m_codeSigningConfigId = jsonValue.GetString("CodeSigningConfigId");

    m_codeSigningConfigIdHasBeenSet = true;
  }

  if(jsonValue.ValueExists("CodeSigningConfigArn"))
  {
    m_codeSigningConfigArn = jsonValue.GetString("CodeSigningConfigArn");

    m_codeSigningConfigArnHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Description"))
  {
    m_description = jsonValue.GetString("Description");

    m_descriptionHasBeenSet = true;
  }

  if(jsonValue.ValueExists("AllowedPublishers"))
  {
    m_allowedPublishers = jsonValue.GetObject("AllowedPublishers");

    m_allowedPublishersHasBeenSet = true;
  }

  if(jsonValue.ValueExists("CodeSigningPolicies"))
  {
    m_codeSigningPolicies = jsonValue.GetObject("CodeSigningPolicies");

    m_codeSigningPoliciesHasBeenSet = true;
  }

  if(jsonValue.ValueExists("LastModified"))
  {
    m_lastModified = jsonValue.GetString("LastModified");

    m_lastModifiedHasBeenSet = true;
  }

  return *this;
}

JsonValue CodeSigningConfig::Jsonize() const
{
  JsonValue payload;

  if(m_codeSigningConfigIdHasBeenSet)
  {
   payload.WithString("CodeSigningConfigId", m_codeSigningConfigId);

  }

  if(m_codeSigningConfigArnHasBeenSet)
  {
   payload.WithString("CodeSigningConfigArn", m_codeSigningConfigArn);

  }

  if(m_descriptionHasBeenSet)
  {
   payload.WithString("Description", m_description);

  }

  if(m_allowedPublishersHasBeenSet)
  {
   payload.WithObject("AllowedPublishers", m_allowedPublishers.Jsonize());

  }

  if(m_codeSigningPoliciesHasBeenSet)
  {
   payload.WithObject("CodeSigningPolicies", m_codeSigningPolicies.Jsonize());

  }

  if(m_lastModifiedHasBeenSet)
  {
   payload.WithString("LastModified", m_lastModified);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
