/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/GetLayerVersionByArnRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

GetLayerVersionByArnRequest::GetLayerVersionByArnRequest() : 
    m_arnHasBeenSet(false)
{
}

Aws::String GetLayerVersionByArnRequest::SerializePayload() const
{
  return {};
}

void GetLayerVersionByArnRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_arnHasBeenSet)
    {
      ss << m_arn;
      uri.AddQueryStringParameter("Arn", ss.str());
      ss.str("");
    }

}



