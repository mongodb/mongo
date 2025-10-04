/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PutUserPermissionsBoundaryRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

PutUserPermissionsBoundaryRequest::PutUserPermissionsBoundaryRequest() : 
    m_userNameHasBeenSet(false),
    m_permissionsBoundaryHasBeenSet(false)
{
}

Aws::String PutUserPermissionsBoundaryRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=PutUserPermissionsBoundary&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_permissionsBoundaryHasBeenSet)
  {
    ss << "PermissionsBoundary=" << StringUtils::URLEncode(m_permissionsBoundary.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  PutUserPermissionsBoundaryRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
