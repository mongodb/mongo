/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetContextKeysForPrincipalPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetContextKeysForPrincipalPolicyRequest::GetContextKeysForPrincipalPolicyRequest() : 
    m_policySourceArnHasBeenSet(false),
    m_policyInputListHasBeenSet(false)
{
}

Aws::String GetContextKeysForPrincipalPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetContextKeysForPrincipalPolicy&";
  if(m_policySourceArnHasBeenSet)
  {
    ss << "PolicySourceArn=" << StringUtils::URLEncode(m_policySourceArn.c_str()) << "&";
  }

  if(m_policyInputListHasBeenSet)
  {
    if (m_policyInputList.empty())
    {
      ss << "PolicyInputList=&";
    }
    else
    {
      unsigned policyInputListCount = 1;
      for(auto& item : m_policyInputList)
      {
        ss << "PolicyInputList.member." << policyInputListCount << "="
            << StringUtils::URLEncode(item.c_str()) << "&";
        policyInputListCount++;
      }
    }
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetContextKeysForPrincipalPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
