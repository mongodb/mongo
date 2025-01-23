/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteServiceLinkedRoleRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteServiceLinkedRoleRequest::DeleteServiceLinkedRoleRequest() : 
    m_roleNameHasBeenSet(false)
{
}

Aws::String DeleteServiceLinkedRoleRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteServiceLinkedRole&";
  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteServiceLinkedRoleRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
