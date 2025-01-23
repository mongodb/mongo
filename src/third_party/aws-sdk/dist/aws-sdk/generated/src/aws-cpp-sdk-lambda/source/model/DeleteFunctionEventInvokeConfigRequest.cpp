/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/DeleteFunctionEventInvokeConfigRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

DeleteFunctionEventInvokeConfigRequest::DeleteFunctionEventInvokeConfigRequest() : 
    m_functionNameHasBeenSet(false),
    m_qualifierHasBeenSet(false)
{
}

Aws::String DeleteFunctionEventInvokeConfigRequest::SerializePayload() const
{
  return {};
}

void DeleteFunctionEventInvokeConfigRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_qualifierHasBeenSet)
    {
      ss << m_qualifier;
      uri.AddQueryStringParameter("Qualifier", ss.str());
      ss.str("");
    }

}



