/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/RemovePermissionRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

RemovePermissionRequest::RemovePermissionRequest() : 
    m_functionNameHasBeenSet(false),
    m_statementIdHasBeenSet(false),
    m_qualifierHasBeenSet(false),
    m_revisionIdHasBeenSet(false)
{
}

Aws::String RemovePermissionRequest::SerializePayload() const
{
  return {};
}

void RemovePermissionRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_qualifierHasBeenSet)
    {
      ss << m_qualifier;
      uri.AddQueryStringParameter("Qualifier", ss.str());
      ss.str("");
    }

    if(m_revisionIdHasBeenSet)
    {
      ss << m_revisionId;
      uri.AddQueryStringParameter("RevisionId", ss.str());
      ss.str("");
    }

}



