/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ListFunctionsRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

ListFunctionsRequest::ListFunctionsRequest() : 
    m_masterRegionHasBeenSet(false),
    m_functionVersion(FunctionVersion::NOT_SET),
    m_functionVersionHasBeenSet(false),
    m_markerHasBeenSet(false),
    m_maxItems(0),
    m_maxItemsHasBeenSet(false)
{
}

Aws::String ListFunctionsRequest::SerializePayload() const
{
  return {};
}

void ListFunctionsRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_masterRegionHasBeenSet)
    {
      ss << m_masterRegion;
      uri.AddQueryStringParameter("MasterRegion", ss.str());
      ss.str("");
    }

    if(m_functionVersionHasBeenSet)
    {
      ss << FunctionVersionMapper::GetNameForFunctionVersion(m_functionVersion);
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



