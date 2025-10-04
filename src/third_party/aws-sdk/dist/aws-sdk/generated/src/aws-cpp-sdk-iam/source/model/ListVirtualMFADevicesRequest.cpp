/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListVirtualMFADevicesRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

ListVirtualMFADevicesRequest::ListVirtualMFADevicesRequest() : 
    m_assignmentStatus(AssignmentStatusType::NOT_SET),
    m_assignmentStatusHasBeenSet(false),
    m_markerHasBeenSet(false),
    m_maxItems(0),
    m_maxItemsHasBeenSet(false)
{
}

Aws::String ListVirtualMFADevicesRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=ListVirtualMFADevices&";
  if(m_assignmentStatusHasBeenSet)
  {
    ss << "AssignmentStatus=" << AssignmentStatusTypeMapper::GetNameForAssignmentStatusType(m_assignmentStatus) << "&";
  }

  if(m_markerHasBeenSet)
  {
    ss << "Marker=" << StringUtils::URLEncode(m_marker.c_str()) << "&";
  }

  if(m_maxItemsHasBeenSet)
  {
    ss << "MaxItems=" << m_maxItems << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  ListVirtualMFADevicesRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
