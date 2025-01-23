/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ListLayerVersionsResult.h>
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

ListLayerVersionsResult::ListLayerVersionsResult()
{
}

ListLayerVersionsResult::ListLayerVersionsResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

ListLayerVersionsResult& ListLayerVersionsResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("NextMarker"))
  {
    m_nextMarker = jsonValue.GetString("NextMarker");

  }

  if(jsonValue.ValueExists("LayerVersions"))
  {
    Aws::Utils::Array<JsonView> layerVersionsJsonList = jsonValue.GetArray("LayerVersions");
    for(unsigned layerVersionsIndex = 0; layerVersionsIndex < layerVersionsJsonList.GetLength(); ++layerVersionsIndex)
    {
      m_layerVersions.push_back(layerVersionsJsonList[layerVersionsIndex].AsObject());
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
