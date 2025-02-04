/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListPoliciesGrantingServiceAccessRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

ListPoliciesGrantingServiceAccessRequest::ListPoliciesGrantingServiceAccessRequest() : 
    m_markerHasBeenSet(false),
    m_arnHasBeenSet(false),
    m_serviceNamespacesHasBeenSet(false)
{
}

Aws::String ListPoliciesGrantingServiceAccessRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=ListPoliciesGrantingServiceAccess&";
  if(m_markerHasBeenSet)
  {
    ss << "Marker=" << StringUtils::URLEncode(m_marker.c_str()) << "&";
  }

  if(m_arnHasBeenSet)
  {
    ss << "Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }

  if(m_serviceNamespacesHasBeenSet)
  {
    if (m_serviceNamespaces.empty())
    {
      ss << "ServiceNamespaces=&";
    }
    else
    {
      unsigned serviceNamespacesCount = 1;
      for(auto& item : m_serviceNamespaces)
      {
        ss << "ServiceNamespaces.member." << serviceNamespacesCount << "="
            << StringUtils::URLEncode(item.c_str()) << "&";
        serviceNamespacesCount++;
      }
    }
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  ListPoliciesGrantingServiceAccessRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
