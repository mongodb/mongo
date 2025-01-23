/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/AttachRolePolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

AttachRolePolicyRequest::AttachRolePolicyRequest() : 
    m_roleNameHasBeenSet(false),
    m_policyArnHasBeenSet(false)
{
}

Aws::String AttachRolePolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=AttachRolePolicy&";
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


void  AttachRolePolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
