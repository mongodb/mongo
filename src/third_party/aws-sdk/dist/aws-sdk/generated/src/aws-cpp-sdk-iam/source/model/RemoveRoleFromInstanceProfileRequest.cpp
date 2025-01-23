/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/RemoveRoleFromInstanceProfileRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

RemoveRoleFromInstanceProfileRequest::RemoveRoleFromInstanceProfileRequest() : 
    m_instanceProfileNameHasBeenSet(false),
    m_roleNameHasBeenSet(false)
{
}

Aws::String RemoveRoleFromInstanceProfileRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=RemoveRoleFromInstanceProfile&";
  if(m_instanceProfileNameHasBeenSet)
  {
    ss << "InstanceProfileName=" << StringUtils::URLEncode(m_instanceProfileName.c_str()) << "&";
  }

  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  RemoveRoleFromInstanceProfileRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
