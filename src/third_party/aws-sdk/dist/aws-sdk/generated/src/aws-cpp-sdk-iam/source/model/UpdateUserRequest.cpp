/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UpdateUserRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UpdateUserRequest::UpdateUserRequest() : 
    m_userNameHasBeenSet(false),
    m_newPathHasBeenSet(false),
    m_newUserNameHasBeenSet(false)
{
}

Aws::String UpdateUserRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UpdateUser&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_newPathHasBeenSet)
  {
    ss << "NewPath=" << StringUtils::URLEncode(m_newPath.c_str()) << "&";
  }

  if(m_newUserNameHasBeenSet)
  {
    ss << "NewUserName=" << StringUtils::URLEncode(m_newUserName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UpdateUserRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
