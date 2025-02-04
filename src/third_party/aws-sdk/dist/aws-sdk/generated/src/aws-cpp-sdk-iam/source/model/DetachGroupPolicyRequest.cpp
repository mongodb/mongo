/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DetachGroupPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DetachGroupPolicyRequest::DetachGroupPolicyRequest() : 
    m_groupNameHasBeenSet(false),
    m_policyArnHasBeenSet(false)
{
}

Aws::String DetachGroupPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DetachGroupPolicy&";
  if(m_groupNameHasBeenSet)
  {
    ss << "GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
  }

  if(m_policyArnHasBeenSet)
  {
    ss << "PolicyArn=" << StringUtils::URLEncode(m_policyArn.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DetachGroupPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
