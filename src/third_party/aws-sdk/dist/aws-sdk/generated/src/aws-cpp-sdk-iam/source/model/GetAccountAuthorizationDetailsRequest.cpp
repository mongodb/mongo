/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetAccountAuthorizationDetailsRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetAccountAuthorizationDetailsRequest::GetAccountAuthorizationDetailsRequest() : 
    m_filterHasBeenSet(false),
    m_maxItems(0),
    m_maxItemsHasBeenSet(false),
    m_markerHasBeenSet(false)
{
}

Aws::String GetAccountAuthorizationDetailsRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetAccountAuthorizationDetails&";
  if(m_filterHasBeenSet)
  {
    if (m_filter.empty())
    {
      ss << "Filter=&";
    }
    else
    {
      unsigned filterCount = 1;
      for(auto& item : m_filter)
      {
        ss << "Filter.member." << filterCount << "="
            << StringUtils::URLEncode(EntityTypeMapper::GetNameForEntityType(item).c_str()) << "&";
        filterCount++;
      }
    }
  }

  if(m_maxItemsHasBeenSet)
  {
    ss << "MaxItems=" << m_maxItems << "&";
  }

  if(m_markerHasBeenSet)
  {
    ss << "Marker=" << StringUtils::URLEncode(m_marker.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetAccountAuthorizationDetailsRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
