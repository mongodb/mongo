/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/PublishLayerVersionRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

PublishLayerVersionRequest::PublishLayerVersionRequest() : 
    m_layerNameHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_contentHasBeenSet(false),
    m_compatibleRuntimesHasBeenSet(false),
    m_licenseInfoHasBeenSet(false),
    m_compatibleArchitecturesHasBeenSet(false)
{
}

Aws::String PublishLayerVersionRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_descriptionHasBeenSet)
  {
   payload.WithString("Description", m_description);

  }

  if(m_contentHasBeenSet)
  {
   payload.WithObject("Content", m_content.Jsonize());

  }

  if(m_compatibleRuntimesHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> compatibleRuntimesJsonList(m_compatibleRuntimes.size());
   for(unsigned compatibleRuntimesIndex = 0; compatibleRuntimesIndex < compatibleRuntimesJsonList.GetLength(); ++compatibleRuntimesIndex)
   {
     compatibleRuntimesJsonList[compatibleRuntimesIndex].AsString(RuntimeMapper::GetNameForRuntime(m_compatibleRuntimes[compatibleRuntimesIndex]));
   }
   payload.WithArray("CompatibleRuntimes", std::move(compatibleRuntimesJsonList));

  }

  if(m_licenseInfoHasBeenSet)
  {
   payload.WithString("LicenseInfo", m_licenseInfo);

  }

  if(m_compatibleArchitecturesHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> compatibleArchitecturesJsonList(m_compatibleArchitectures.size());
   for(unsigned compatibleArchitecturesIndex = 0; compatibleArchitecturesIndex < compatibleArchitecturesJsonList.GetLength(); ++compatibleArchitecturesIndex)
   {
     compatibleArchitecturesJsonList[compatibleArchitecturesIndex].AsString(ArchitectureMapper::GetNameForArchitecture(m_compatibleArchitectures[compatibleArchitecturesIndex]));
   }
   payload.WithArray("CompatibleArchitectures", std::move(compatibleArchitecturesJsonList));

  }

  return payload.View().WriteReadable();
}




