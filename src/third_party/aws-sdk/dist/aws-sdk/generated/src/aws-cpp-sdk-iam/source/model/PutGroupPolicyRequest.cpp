/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PutGroupPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

PutGroupPolicyRequest::PutGroupPolicyRequest() : 
    m_groupNameHasBeenSet(false),
    m_policyNameHasBeenSet(false),
    m_policyDocumentHasBeenSet(false)
{
}

Aws::String PutGroupPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=PutGroupPolicy&";
  if(m_groupNameHasBeenSet)
  {
    ss << "GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
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


void  PutGroupPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
