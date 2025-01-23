/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PutRolePolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

PutRolePolicyRequest::PutRolePolicyRequest() : 
    m_roleNameHasBeenSet(false),
    m_policyNameHasBeenSet(false),
    m_policyDocumentHasBeenSet(false)
{
}

Aws::String PutRolePolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=PutRolePolicy&";
  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  if(m_policyNameHasBeenSet)
  {
    ss << "PolicyName=" << StringUtils::URLEncode(m_policyName.c_str()) << "&";
  }

  if(m_policyDocumentHasBeenSet)
  {
    ss << "PolicyDocument=" << StringUtils::URLEncode(m_policyDocument.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  PutRolePolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
