/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetServiceLinkedRoleDeletionStatusRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetServiceLinkedRoleDeletionStatusRequest::GetServiceLinkedRoleDeletionStatusRequest() : 
    m_deletionTaskIdHasBeenSet(false)
{
}

Aws::String GetServiceLinkedRoleDeletionStatusRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetServiceLinkedRoleDeletionStatus&";
  if(m_deletionTaskIdHasBeenSet)
  {
    ss << "DeletionTaskId=" << StringUtils::URLEncode(m_deletionTaskId.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetServiceLinkedRoleDeletionStatusRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
