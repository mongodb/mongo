/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/UpdateCodeSigningConfigRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

UpdateCodeSigningConfigRequest::UpdateCodeSigningConfigRequest() : 
    m_codeSigningConfigArnHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_allowedPublishersHasBeenSet(false),
    m_codeSigningPoliciesHasBeenSet(false)
{
}

Aws::String UpdateCodeSigningConfigRequest::SerializePayload() const
{
  JsonValue payload;

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

  return payload.View().WriteReadable();
}




