/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetServiceLastAccessedDetailsRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetServiceLastAccessedDetailsRequest::GetServiceLastAccessedDetailsRequest() : 
    m_jobIdHasBeenSet(false),
    m_maxItems(0),
    m_maxItemsHasBeenSet(false),
    m_markerHasBeenSet(false)
{
}

Aws::String GetServiceLastAccessedDetailsRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetServiceLastAccessedDetails&";
  if(m_jobIdHasBeenSet)
  {
    ss << "JobId=" << StringUtils::URLEncode(m_jobId.c_str()) << "&";
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


void  GetServiceLastAccessedDetailsRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
