/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UpdateGroupRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UpdateGroupRequest::UpdateGroupRequest() : 
    m_groupNameHasBeenSet(false),
    m_newPathHasBeenSet(false),
    m_newGroupNameHasBeenSet(false)
{
}

Aws::String UpdateGroupRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UpdateGroup&";
  if(m_groupNameHasBeenSet)
  {
    ss << "GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
  }

  if(m_newPathHasBeenSet)
  {
    ss << "NewPath=" << StringUtils::URLEncode(m_newPath.c_str()) << "&";
  }

  if(m_newGroupNameHasBeenSet)
  {
    ss << "NewGroupName=" << StringUtils::URLEncode(m_newGroupName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UpdateGroupRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
