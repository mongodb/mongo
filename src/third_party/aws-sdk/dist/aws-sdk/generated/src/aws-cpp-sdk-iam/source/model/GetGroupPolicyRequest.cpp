/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetGroupPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetGroupPolicyRequest::GetGroupPolicyRequest() : 
    m_groupNameHasBeenSet(false),
    m_policyNameHasBeenSet(false)
{
}

Aws::String GetGroupPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetGroupPolicy&";
  if(m_groupNameHasBeenSet)
  {
    ss << "GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
  }

  if(m_policyNameHasBeenSet)
  {
    ss << "PolicyName=" << StringUtils::URLEncode(m_policyName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetGroupPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
