/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/CreateAliasRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

CreateAliasRequest::CreateAliasRequest() : 
    m_functionNameHasBeenSet(false),
    m_nameHasBeenSet(false),
    m_functionVersionHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_routingConfigHasBeenSet(false)
{
}

Aws::String CreateAliasRequest::SerializePayload() const
{
  JsonValue payload;

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

  return payload.View().WriteReadable();
}




