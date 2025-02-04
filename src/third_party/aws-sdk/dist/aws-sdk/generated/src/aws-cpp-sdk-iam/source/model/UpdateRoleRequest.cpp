/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UpdateRoleRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UpdateRoleRequest::UpdateRoleRequest() : 
    m_roleNameHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_maxSessionDuration(0),
    m_maxSessionDurationHasBeenSet(false)
{
}

Aws::String UpdateRoleRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UpdateRole&";
  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  if(m_descriptionHasBeenSet)
  {
    ss << "Description=" << StringUtils::URLEncode(m_description.c_str()) << "&";
  }

  if(m_maxSessionDurationHasBeenSet)
  {
    ss << "MaxSessionDuration=" << m_maxSessionDuration << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UpdateRoleRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
