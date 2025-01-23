/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DetachRolePolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DetachRolePolicyRequest::DetachRolePolicyRequest() : 
    m_roleNameHasBeenSet(false),
    m_policyArnHasBeenSet(false)
{
}

Aws::String DetachRolePolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DetachRolePolicy&";
  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  if(m_policyArnHasBeenSet)
  {
    ss << "PolicyArn=" << StringUtils::URLEncode(m_policyArn.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DetachRolePolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
