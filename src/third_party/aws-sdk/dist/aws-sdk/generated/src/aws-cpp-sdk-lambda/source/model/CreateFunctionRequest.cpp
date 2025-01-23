/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/CreateFunctionRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

CreateFunctionRequest::CreateFunctionRequest() : 
    m_functionNameHasBeenSet(false),
    m_runtime(Runtime::NOT_SET),
    m_runtimeHasBeenSet(false),
    m_roleHasBeenSet(false),
    m_handlerHasBeenSet(false),
    m_codeHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_timeout(0),
    m_timeoutHasBeenSet(false),
    m_memorySize(0),
    m_memorySizeHasBeenSet(false),
    m_publish(false),
    m_publishHasBeenSet(false),
    m_vpcConfigHasBeenSet(false),
    m_packageType(PackageType::NOT_SET),
    m_packageTypeHasBeenSet(false),
    m_deadLetterConfigHasBeenSet(false),
    m_environmentHasBeenSet(false),
    m_kMSKeyArnHasBeenSet(false),
    m_tracingConfigHasBeenSet(false),
    m_tagsHasBeenSet(false),
    m_layersHasBeenSet(false),
    m_fileSystemConfigsHasBeenSet(false),
    m_imageConfigHasBeenSet(false),
    m_codeSigningConfigArnHasBeenSet(false),
    m_architecturesHasBeenSet(false),
    m_ephemeralStorageHasBeenSet(false),
    m_snapStartHasBeenSet(false),
    m_loggingConfigHasBeenSet(false)
{
}

Aws::String CreateFunctionRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_functionNameHasBeenSet)
  {
   payload.WithString("FunctionName", m_functionName);

  }

  if(m_runtimeHasBeenSet)
  {
   payload.WithString("Runtime", RuntimeMapper::GetNameForRuntime(m_runtime));
  }

  if(m_roleHasBeenSet)
  {
   payload.WithString("Role", m_role);

  }

  if(m_handlerHasBeenSet)
  {
   payload.WithString("Handler", m_handler);

  }

  if(m_codeHasBeenSet)
  {
   payload.WithObject("Code", m_code.Jsonize());

  }

  if(m_descriptionHasBeenSet)
  {
   payload.WithString("Description", m_description);

  }

  if(m_timeoutHasBeenSet)
  {
   payload.WithInteger("Timeout", m_timeout);

  }

  if(m_memorySizeHasBeenSet)
  {
   payload.WithInteger("MemorySize", m_memorySize);

  }

  if(m_publishHasBeenSet)
  {
   payload.WithBool("Publish", m_publish);

  }

  if(m_vpcConfigHasBeenSet)
  {
   payload.WithObject("VpcConfig", m_vpcConfig.Jsonize());

  }

  if(m_packageTypeHasBeenSet)
  {
   payload.WithString("PackageType", PackageTypeMapper::GetNameForPackageType(m_packageType));
  }

  if(m_deadLetterConfigHasBeenSet)
  {
   payload.WithObject("DeadLetterConfig", m_deadLetterConfig.Jsonize());

  }

  if(m_environmentHasBeenSet)
  {
   payload.WithObject("Environment", m_environment.Jsonize());

  }

  if(m_kMSKeyArnHasBeenSet)
  {
   payload.WithString("KMSKeyArn", m_kMSKeyArn);

  }

  if(m_tracingConfigHasBeenSet)
  {
   payload.WithObject("TracingConfig", m_tracingConfig.Jsonize());

  }

  if(m_tagsHasBeenSet)
  {
   JsonValue tagsJsonMap;
   for(auto& tagsItem : m_tags)
   {
     tagsJsonMap.WithString(tagsItem.first, tagsItem.second);
   }
   payload.WithObject("Tags", std::move(tagsJsonMap));

  }

  if(m_layersHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> layersJsonList(m_layers.size());
   for(unsigned layersIndex = 0; layersIndex < layersJsonList.GetLength(); ++layersIndex)
   {
     layersJsonList[layersIndex].AsString(m_layers[layersIndex]);
   }
   payload.WithArray("Layers", std::move(layersJsonList));

  }

  if(m_fileSystemConfigsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> fileSystemConfigsJsonList(m_fileSystemConfigs.size());
   for(unsigned fileSystemConfigsIndex = 0; fileSystemConfigsIndex < fileSystemConfigsJsonList.GetLength(); ++fileSystemConfigsIndex)
   {
     fileSystemConfigsJsonList[fileSystemConfigsIndex].AsObject(m_fileSystemConfigs[fileSystemConfigsIndex].Jsonize());
   }
   payload.WithArray("FileSystemConfigs", std::move(fileSystemConfigsJsonList));

  }

  if(m_imageConfigHasBeenSet)
  {
   payload.WithObject("ImageConfig", m_imageConfig.Jsonize());

  }

  if(m_codeSigningConfigArnHasBeenSet)
  {
   payload.WithString("CodeSigningConfigArn", m_codeSigningConfigArn);

  }

  if(m_architecturesHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> architecturesJsonList(m_architectures.size());
   for(unsigned architecturesIndex = 0; architecturesIndex < architecturesJsonList.GetLength(); ++architecturesIndex)
   {
     architecturesJsonList[architecturesIndex].AsString(ArchitectureMapper::GetNameForArchitecture(m_architectures[architecturesIndex]));
   }
   payload.WithArray("Architectures", std::move(architecturesJsonList));

  }

  if(m_ephemeralStorageHasBeenSet)
  {
   payload.WithObject("EphemeralStorage", m_ephemeralStorage.Jsonize());

  }

  if(m_snapStartHasBeenSet)
  {
   payload.WithObject("SnapStart", m_snapStart.Jsonize());

  }

  if(m_loggingConfigHasBeenSet)
  {
   payload.WithObject("LoggingConfig", m_loggingConfig.Jsonize());

  }

  return payload.View().WriteReadable();
}




