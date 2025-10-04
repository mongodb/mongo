/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PutRolePermissionsBoundaryRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

PutRolePermissionsBoundaryRequest::PutRolePermissionsBoundaryRequest() : 
    m_roleNameHasBeenSet(false),
    m_permissionsBoundaryHasBeenSet(false)
{
}

Aws::String PutRolePermissionsBoundaryRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=PutRolePermissionsBoundary&";
  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  if(m_permissionsBoundaryHasBeenSet)
  {
    ss << "PermissionsBoundary=" << StringUtils::URLEncode(m_permissionsBoundary.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  PutRolePermissionsBoundaryRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
