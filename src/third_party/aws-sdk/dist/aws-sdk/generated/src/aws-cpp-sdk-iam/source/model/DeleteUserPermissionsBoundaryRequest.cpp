/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteUserPermissionsBoundaryRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteUserPermissionsBoundaryRequest::DeleteUserPermissionsBoundaryRequest() : 
    m_userNameHasBeenSet(false)
{
}

Aws::String DeleteUserPermissionsBoundaryRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteUserPermissionsBoundary&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteUserPermissionsBoundaryRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
