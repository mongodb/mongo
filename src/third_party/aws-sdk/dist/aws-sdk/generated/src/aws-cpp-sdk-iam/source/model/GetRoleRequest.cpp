/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetRoleRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetRoleRequest::GetRoleRequest() : 
    m_roleNameHasBeenSet(false)
{
}

Aws::String GetRoleRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetRole&";
  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetRoleRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
