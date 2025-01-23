/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ListAliasesRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

ListAliasesRequest::ListAliasesRequest() : 
    m_functionNameHasBeenSet(false),
    m_functionVersionHasBeenSet(false),
    m_markerHasBeenSet(false),
    m_maxItems(0),
    m_maxItemsHasBeenSet(false)
{
}

Aws::String ListAliasesRequest::SerializePayload() const
{
  return {};
}

void ListAliasesRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_functionVersionHasBeenSet)
    {
      ss << m_functionVersion;
      uri.AddQueryStringParameter("FunctionVersion", ss.str());
      ss.str("");
    }

    if(m_markerHasBeenSet)
    {
      ss << m_marker;
      uri.AddQueryStringParameter("Marker", ss.str());
      ss.str("");
    }

    if(m_maxItemsHasBeenSet)
    {
      ss << m_maxItems;
      uri.AddQueryStringParameter("MaxItems", ss.str());
      ss.str("");
    }

}



