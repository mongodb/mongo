/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/RemoveLayerVersionPermissionRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

RemoveLayerVersionPermissionRequest::RemoveLayerVersionPermissionRequest() : 
    m_layerNameHasBeenSet(false),
    m_versionNumber(0),
    m_versionNumberHasBeenSet(false),
    m_statementIdHasBeenSet(false),
    m_revisionIdHasBeenSet(false)
{
}

Aws::String RemoveLayerVersionPermissionRequest::SerializePayload() const
{
  return {};
}

void RemoveLayerVersionPermissionRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_revisionIdHasBeenSet)
    {
      ss << m_revisionId;
      uri.AddQueryStringParameter("RevisionId", ss.str());
      ss.str("");
    }

}



