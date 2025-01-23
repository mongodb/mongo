/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetContextKeysForCustomPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetContextKeysForCustomPolicyRequest::GetContextKeysForCustomPolicyRequest() : 
    m_policyInputListHasBeenSet(false)
{
}

Aws::String GetContextKeysForCustomPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetContextKeysForCustomPolicy&";
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


void  GetContextKeysForCustomPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
