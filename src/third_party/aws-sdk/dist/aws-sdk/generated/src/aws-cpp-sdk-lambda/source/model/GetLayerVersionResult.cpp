/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/GetLayerVersionResult.h>
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

GetLayerVersionResult::GetLayerVersionResult() : 
    m_version(0)
{
}

GetLayerVersionResult::GetLayerVersionResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
  : GetLayerVersionResult()
{
  *this = result;
}

GetLayerVersionResult& GetLayerVersionResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("Content"))
  {
    m_content = jsonValue.GetObject("Content");

  }

  if(jsonValue.ValueExists("LayerArn"))
  {
    m_layerArn = jsonValue.GetString("LayerArn");

  }

  if(jsonValue.ValueExists("LayerVersionArn"))
  {
    m_layerVersionArn = jsonValue.GetString("LayerVersionArn");

  }

  if(jsonValue.ValueExists("Description"))
  {
    m_description = jsonValue.GetString("Description");

  }

  if(jsonValue.ValueExists("CreatedDate"))
  {
    m_createdDate = jsonValue.GetString("CreatedDate");

  }

  if(jsonValue.ValueExists("Version"))
  {
    m_version = jsonValue.GetInt64("Version");

  }

  if(jsonValue.ValueExists("CompatibleRuntimes"))
  {
    Aws::Utils::Array<JsonView> compatibleRuntimesJsonList = jsonValue.GetArray("CompatibleRuntimes");
    for(unsigned compatibleRuntimesIndex = 0; compatibleRuntimesIndex < compatibleRuntimesJsonList.GetLength(); ++compatibleRuntimesIndex)
    {
      m_compatibleRuntimes.push_back(RuntimeMapper::GetRuntimeForName(compatibleRuntimesJsonList[compatibleRuntimesIndex].AsString()));
    }
  }

  if(jsonValue.ValueExists("LicenseInfo"))
  {
    m_licenseInfo = jsonValue.GetString("LicenseInfo");

  }

  if(jsonValue.ValueExists("CompatibleArchitectures"))
  {
    Aws::Utils::Array<JsonView> compatibleArchitecturesJsonList = jsonValue.GetArray("CompatibleArchitectures");
    for(unsigned compatibleArchitecturesIndex = 0; compatibleArchitecturesIndex < compatibleArchitecturesJsonList.GetLength(); ++compatibleArchitecturesIndex)
    {
      m_compatibleArchitectures.push_back(ArchitectureMapper::GetArchitectureForName(compatibleArchitecturesJsonList[compatibleArchitecturesIndex].AsString()));
    }
  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
