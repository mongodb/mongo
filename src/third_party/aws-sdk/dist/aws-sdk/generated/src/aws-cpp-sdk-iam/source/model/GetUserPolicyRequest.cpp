/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetUserPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetUserPolicyRequest::GetUserPolicyRequest() : 
    m_userNameHasBeenSet(false),
    m_policyNameHasBeenSet(false)
{
}

Aws::String GetUserPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetUserPolicy&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_policyNameHasBeenSet)
  {
    ss << "PolicyName=" << StringUtils::URLEncode(m_policyName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetUserPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
