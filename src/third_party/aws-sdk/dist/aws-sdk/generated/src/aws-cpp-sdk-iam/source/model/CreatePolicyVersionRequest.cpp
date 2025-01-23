/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreatePolicyVersionRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreatePolicyVersionRequest::CreatePolicyVersionRequest() : 
    m_policyArnHasBeenSet(false),
    m_policyDocumentHasBeenSet(false),
    m_setAsDefault(false),
    m_setAsDefaultHasBeenSet(false)
{
}

Aws::String CreatePolicyVersionRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreatePolicyVersion&";
  if(m_policyArnHasBeenSet)
  {
    ss << "PolicyArn=" << StringUtils::URLEncode(m_policyArn.c_str()) << "&";
  }

  if(m_policyDocumentHasBeenSet)
  {
    ss << "PolicyDocument=" << StringUtils::URLEncode(m_policyDocument.c_str()) << "&";
  }

  if(m_setAsDefaultHasBeenSet)
  {
    ss << "SetAsDefault=" << std::boolalpha << m_setAsDefault << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  CreatePolicyVersionRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
