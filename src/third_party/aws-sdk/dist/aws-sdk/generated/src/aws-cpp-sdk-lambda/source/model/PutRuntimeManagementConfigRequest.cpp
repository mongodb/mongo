/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/PutRuntimeManagementConfigRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

PutRuntimeManagementConfigRequest::PutRuntimeManagementConfigRequest() : 
    m_functionNameHasBeenSet(false),
    m_qualifierHasBeenSet(false),
    m_updateRuntimeOn(UpdateRuntimeOn::NOT_SET),
    m_updateRuntimeOnHasBeenSet(false),
    m_runtimeVersionArnHasBeenSet(false)
{
}

Aws::String PutRuntimeManagementConfigRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_updateRuntimeOnHasBeenSet)
  {
   payload.WithString("UpdateRuntimeOn", UpdateRuntimeOnMapper::GetNameForUpdateRuntimeOn(m_updateRuntimeOn));
  }

  if(m_runtimeVersionArnHasBeenSet)
  {
   payload.WithString("RuntimeVersionArn", m_runtimeVersionArn);

  }

  return payload.View().WriteReadable();
}

void PutRuntimeManagementConfigRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_qualifierHasBeenSet)
    {
      ss << m_qualifier;
      uri.AddQueryStringParameter("Qualifier", ss.str());
      ss.str("");
    }

}



