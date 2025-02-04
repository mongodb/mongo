/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ListEventSourceMappingsRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;
using namespace Aws::Http;

ListEventSourceMappingsRequest::ListEventSourceMappingsRequest() : 
    m_eventSourceArnHasBeenSet(false),
    m_functionNameHasBeenSet(false),
    m_markerHasBeenSet(false),
    m_maxItems(0),
    m_maxItemsHasBeenSet(false)
{
}

Aws::String ListEventSourceMappingsRequest::SerializePayload() const
{
  return {};
}

void ListEventSourceMappingsRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_eventSourceArnHasBeenSet)
    {
      ss << m_eventSourceArn;
      uri.AddQueryStringParameter("EventSourceArn", ss.str());
      ss.str("");
    }

    if(m_functionNameHasBeenSet)
    {
      ss << m_functionName;
      uri.AddQueryStringParameter("FunctionName", ss.str());
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



