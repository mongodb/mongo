/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/AliasConfiguration.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

AliasConfiguration::AliasConfiguration() : 
    m_aliasArnHasBeenSet(false),
    m_nameHasBeenSet(false),
    m_functionVersionHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_routingConfigHasBeenSet(false),
    m_revisionIdHasBeenSet(false),
    m_requestIdHasBeenSet(false)
{
}

AliasConfiguration::AliasConfiguration(JsonView jsonValue)
  : AliasConfiguration()
{
  *this = jsonValue;
}

AliasConfiguration& AliasConfiguration::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("AliasArn"))
  {
    m_aliasArn = jsonValue.GetString("AliasArn");

    m_aliasArnHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Name"))
  {
    m_name = jsonValue.GetString("Name");

    m_nameHasBeenSet = true;
  }

  if(jsonValue.ValueExists("FunctionVersion"))
  {
    m_functionVersion = jsonValue.GetString("FunctionVersion");

    m_functionVersionHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Description"))
  {
    m_description = jsonValue.GetString("Description");

    m_descriptionHasBeenSet = true;
  }

  if(jsonValue.ValueExists("RoutingConfig"))
  {
    m_routingConfig = jsonValue.GetObject("RoutingConfig");

    m_routingConfigHasBeenSet = true;
  }

  if(jsonValue.ValueExists("RevisionId"))
  {
    m_revisionId = jsonValue.GetString("RevisionId");

    m_revisionIdHasBeenSet = true;
  }

  return *this;
}

JsonValue AliasConfiguration::Jsonize() const
{
  JsonValue payload;

  if(m_aliasArnHasBeenSet)
  {
   payload.WithString("AliasArn", m_aliasArn);

  }

  if(m_nameHasBeenSet)
  {
   payload.WithString("Name", m_name);

  }

  if(m_functionVersionHasBeenSet)
  {
   payload.WithString("FunctionVersion", m_functionVersion);

  }

  if(m_descriptionHasBeenSet)
  {
   payload.WithString("Description", m_description);

  }

  if(m_routingConfigHasBeenSet)
  {
   payload.WithObject("RoutingConfig", m_routingConfig.Jsonize());

  }

  if(m_revisionIdHasBeenSet)
  {
   payload.WithString("RevisionId", m_revisionId);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
