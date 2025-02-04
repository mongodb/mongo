/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/UpdateFunctionConfigurationResult.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws;

UpdateFunctionConfigurationResult::UpdateFunctionConfigurationResult() : 
    m_runtime(Runtime::NOT_SET),
    m_codeSize(0),
    m_timeout(0),
    m_memorySize(0),
    m_state(State::NOT_SET),
    m_stateReasonCode(StateReasonCode::NOT_SET),
    m_lastUpdateStatus(LastUpdateStatus::NOT_SET),
    m_lastUpdateStatusReasonCode(LastUpdateStatusReasonCode::NOT_SET),
    m_packageType(PackageType::NOT_SET)
{
}

UpdateFunctionConfigurationResult::UpdateFunctionConfigurationResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : UpdateFunctionConfigurationResult()
{
  *this = result;
}

UpdateFunctionConfigurationResult& UpdateFunctionConfigurationResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("FunctionName"))
  {
    m_functionName = jsonValue.GetString("FunctionName");

  }

  if(jsonValue.ValueExists("FunctionArn"))
  {
    m_functionArn = jsonValue.GetString("FunctionArn");

  }

  if(jsonValue.ValueExists("Runtime"))
  {
    m_runtime = RuntimeMapper::GetRuntimeForName(jsonValue.GetString("Runtime"));

  }

  if(jsonValue.ValueExists("Role"))
  {
    m_role = jsonValue.GetString("Role");

  }

  if(jsonValue.ValueExists("Handler"))
  {
    m_handler = jsonValue.GetString("Handler");

  }

  if(jsonValue.ValueExists("CodeSize"))
  {
    m_codeSize = jsonValue.GetInt64("CodeSize");

  }

  if(jsonValue.ValueExists("Description"))
  {
    m_description = jsonValue.GetString("Description");

  }

  if(jsonValue.ValueExists("Timeout"))
  {
    m_timeout = jsonValue.GetInteger("Timeout");

  }

  if(jsonValue.ValueExists("MemorySize"))
  {
    m_memorySize = jsonValue.GetInteger("MemorySize");

  }

  if(jsonValue.ValueExists("LastModified"))
  {
    m_lastModified = jsonValue.GetString("LastModified");

  }

  if(jsonValue.ValueExists("CodeSha256"))
  {
    m_codeSha256 = jsonValue.GetString("CodeSha256");

  }

  if(jsonValue.ValueExists("Version"))
  {
    m_version = jsonValue.GetString("Version");

  }

  if(jsonValue.ValueExists("VpcConfig"))
  {
    m_vpcConfig = jsonValue.GetObject("VpcConfig");

  }

  if(jsonValue.ValueExists("DeadLetterConfig"))
  {
    m_deadLetterConfig = jsonValue.GetObject("DeadLetterConfig");

  }

  if(jsonValue.ValueExists("Environment"))
  {
    m_environment = jsonValue.GetObject("Environment");

  }

  if(jsonValue.ValueExists("KMSKeyArn"))
  {
    m_kMSKeyArn = jsonValue.GetString("KMSKeyArn");

  }

  if(jsonValue.ValueExists("TracingConfig"))
  {
    m_tracingConfig = jsonValue.GetObject("TracingConfig");

  }

  if(jsonValue.ValueExists("MasterArn"))
  {
    m_masterArn = jsonValue.GetString("MasterArn");

  }

  if(jsonValue.ValueExists("RevisionId"))
  {
    m_revisionId = jsonValue.GetString("RevisionId");

  }

  if(jsonValue.ValueExists("Layers"))
  {
    Aws::Utils::Array<JsonView> layersJsonList = jsonValue.GetArray("Layers");
    for(unsigned layersIndex = 0; layersIndex < layersJsonList.GetLength(); ++layersIndex)
    {
      m_layers.push_back(layersJsonList[layersIndex].AsObject());
    }
  }

  if(jsonValue.ValueExists("State"))
  {
    m_state = StateMapper::GetStateForName(jsonValue.GetString("State"));

  }

  if(jsonValue.ValueExists("StateReason"))
  {
    m_stateReason = jsonValue.GetString("StateReason");

  }

  if(jsonValue.ValueExists("StateReasonCode"))
  {
    m_stateReasonCode = StateReasonCodeMapper::GetStateReasonCodeForName(jsonValue.GetString("StateReasonCode"));

  }

  if(jsonValue.ValueExists("LastUpdateStatus"))
  {
    m_lastUpdateStatus = LastUpdateStatusMapper::GetLastUpdateStatusForName(jsonValue.GetString("LastUpdateStatus"));

  }

  if(jsonValue.ValueExists("LastUpdateStatusReason"))
  {
    m_lastUpdateStatusReason = jsonValue.GetString("LastUpdateStatusReason");

  }

  if(jsonValue.ValueExists("LastUpdateStatusReasonCode"))
  {
    m_lastUpdateStatusReasonCode = LastUpdateStatusReasonCodeMapper::GetLastUpdateStatusReasonCodeForName(jsonValue.GetString("LastUpdateStatusReasonCode"));

  }

  if(jsonValue.ValueExists("FileSystemConfigs"))
  {
    Aws::Utils::Array<JsonView> fileSystemConfigsJsonList = jsonValue.GetArray("FileSystemConfigs");
    for(unsigned fileSystemConfigsIndex = 0; fileSystemConfigsIndex < fileSystemConfigsJsonList.GetLength(); ++fileSystemConfigsIndex)
    {
      m_fileSystemConfigs.push_back(fileSystemConfigsJsonList[fileSystemConfigsIndex].AsObject());
    }
  }

  if(jsonValue.ValueExists("PackageType"))
  {
    m_packageType = PackageTypeMapper::GetPackageTypeForName(jsonValue.GetString("PackageType"));

  }

  if(jsonValue.ValueExists("ImageConfigResponse"))
  {
    m_imageConfigResponse = jsonValue.GetObject("ImageConfigResponse");

  }

  if(jsonValue.ValueExists("SigningProfileVersionArn"))
  {
    m_signingProfileVersionArn = jsonValue.GetString("SigningProfileVersionArn");

  }

  if(jsonValue.ValueExists("SigningJobArn"))
  {
    m_signingJobArn = jsonValue.GetString("SigningJobArn");

  }

  if(jsonValue.ValueExists("Architectures"))
  {
    Aws::Utils::Array<JsonView> architecturesJsonList = jsonValue.GetArray("Architectures");
    for(unsigned architecturesIndex = 0; architecturesIndex < architecturesJsonList.GetLength(); ++architecturesIndex)
    {
      m_architectures.push_back(ArchitectureMapper::GetArchitectureForName(architecturesJsonList[architecturesIndex].AsString()));
    }
  }

  if(jsonValue.ValueExists("EphemeralStorage"))
  {
    m_ephemeralStorage = jsonValue.GetObject("EphemeralStorage");

  }

  if(jsonValue.ValueExists("SnapStart"))
  {
    m_snapStart = jsonValue.GetObject("SnapStart");

  }

  if(jsonValue.ValueExists("RuntimeVersionConfig"))
  {
    m_runtimeVersionConfig = jsonValue.GetObject("RuntimeVersionConfig");

  }

  if(jsonValue.ValueExists("LoggingConfig"))
  {
    m_loggingConfig = jsonValue.GetObject("LoggingConfig");

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
