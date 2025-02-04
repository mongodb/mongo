/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/GetFunctionResult.h>
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

GetFunctionResult::GetFunctionResult()
{
}

GetFunctionResult::GetFunctionResult(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  *this = result;
}

GetFunctionResult& GetFunctionResult::operator =(const Aws::AmazonWebServiceResult<JsonValue>& result)
{
  JsonView jsonValue = result.GetPayload().View();
  if(jsonValue.ValueExists("Configuration"))
  {
    m_configuration = jsonValue.GetObject("Configuration");

  }

  if(jsonValue.ValueExists("Code"))
  {
    m_code = jsonValue.GetObject("Code");

  }

  if(jsonValue.ValueExists("Tags"))
  {
    Aws::Map<Aws::String, JsonView> tagsJsonMap = jsonValue.GetObject("Tags").GetAllObjects();
    for(auto& tagsItem : tagsJsonMap)
    {
      m_tags[tagsItem.first] = tagsItem.second.AsString();
    }
  }

  if(jsonValue.ValueExists("TagsError"))
  {
    m_tagsError = jsonValue.GetObject("TagsError");

  }

  if(jsonValue.ValueExists("Concurrency"))
  {
    m_concurrency = jsonValue.GetObject("Concurrency");

  }


  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amzn-requestid");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }


  return *this;
}
