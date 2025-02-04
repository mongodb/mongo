/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteRolePermissionsBoundaryRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteRolePermissionsBoundaryRequest::DeleteRolePermissionsBoundaryRequest() : 
    m_roleNameHasBeenSet(false)
{
}

Aws::String DeleteRolePermissionsBoundaryRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteRolePermissionsBoundary&";
  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteRolePermissionsBoundaryRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
