/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/CreateFunctionUrlConfigRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

CreateFunctionUrlConfigRequest::CreateFunctionUrlConfigRequest() : 
    m_functionNameHasBeenSet(false),
    m_qualifierHasBeenSet(false),
    m_authType(FunctionUrlAuthType::NOT_SET),
    m_authTypeHasBeenSet(false),
    m_corsHasBeenSet(false),
    m_invokeMode(InvokeMode::NOT_SET),
    m_invokeModeHasBeenSet(false)
{
}

Aws::String CreateFunctionUrlConfigRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_authTypeHasBeenSet)
  {
   payload.WithString("AuthType", FunctionUrlAuthTypeMapper::GetNameForFunctionUrlAuthType(m_authType));
  }

  if(m_corsHasBeenSet)
  {
   payload.WithObject("Cors", m_cors.Jsonize());

  }

  if(m_invokeModeHasBeenSet)
  {
   payload.WithString("InvokeMode", InvokeModeMapper::GetNameForInvokeMode(m_invokeMode));
  }

  return payload.View().WriteReadable();
}

void CreateFunctionUrlConfigRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_qualifierHasBeenSet)
    {
      ss << m_qualifier;
      uri.AddQueryStringParameter("Qualifier", ss.str());
      ss.str("");
    }

}



