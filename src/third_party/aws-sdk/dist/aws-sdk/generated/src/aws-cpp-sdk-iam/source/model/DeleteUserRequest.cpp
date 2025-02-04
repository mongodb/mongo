/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteUserRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteUserRequest::DeleteUserRequest() : 
    m_userNameHasBeenSet(false)
{
}

Aws::String DeleteUserRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteUser&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteUserRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
