/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/InvokeWithResponseStreamInitialResponse.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UnreferencedParam.h>
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

InvokeWithResponseStreamInitialResponse::InvokeWithResponseStreamInitialResponse() : 
    m_statusCode(0),
    m_statusCodeHasBeenSet(false),
    m_executedVersionHasBeenSet(false),
    m_responseStreamContentTypeHasBeenSet(false)
{
}

InvokeWithResponseStreamInitialResponse::InvokeWithResponseStreamInitialResponse(JsonView jsonValue)
  : InvokeWithResponseStreamInitialResponse()
{
  *this = jsonValue;
}

InvokeWithResponseStreamInitialResponse& InvokeWithResponseStreamInitialResponse::operator =(JsonView jsonValue)
{
  AWS_UNREFERENCED_PARAM(jsonValue);
  return *this;
}

InvokeWithResponseStreamInitialResponse::InvokeWithResponseStreamInitialResponse(const Http::HeaderValueCollection& headers) : InvokeWithResponseStreamInitialResponse()
{
  const auto& executedVersionIter = headers.find("x-amz-executed-version");
  if(executedVersionIter != headers.end())
  {
    m_executedVersion = executedVersionIter->second;
  }

  const auto& responseStreamContentTypeIter = headers.find("content-type");
  if(responseStreamContentTypeIter != headers.end())
  {
    m_responseStreamContentType = responseStreamContentTypeIter->second;
  }

}

JsonValue InvokeWithResponseStreamInitialResponse::Jsonize() const
{
  JsonValue payload;

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
