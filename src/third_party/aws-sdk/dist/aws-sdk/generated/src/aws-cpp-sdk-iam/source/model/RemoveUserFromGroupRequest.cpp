/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/RemoveUserFromGroupRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

RemoveUserFromGroupRequest::RemoveUserFromGroupRequest() : 
    m_groupNameHasBeenSet(false),
    m_userNameHasBeenSet(false)
{
}

Aws::String RemoveUserFromGroupRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=RemoveUserFromGroup&";
  if(m_groupNameHasBeenSet)
  {
    ss << "GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
  }

  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  RemoveUserFromGroupRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
