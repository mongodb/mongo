/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UntagPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UntagPolicyRequest::UntagPolicyRequest() : 
    m_policyArnHasBeenSet(false),
    m_tagKeysHasBeenSet(false)
{
}

Aws::String UntagPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UntagPolicy&";
  if(m_policyArnHasBeenSet)
  {
    ss << "PolicyArn=" << StringUtils::URLEncode(m_policyArn.c_str()) << "&";
  }

  if(m_tagKeysHasBeenSet)
  {
    if (m_tagKeys.empty())
    {
      ss << "TagKeys=&";
    }
    else
    {
      unsigned tagKeysCount = 1;
      for(auto& item : m_tagKeys)
      {
        ss << "TagKeys.member." << tagKeysCount << "="
            << StringUtils::URLEncode(item.c_str()) << "&";
        tagKeysCount++;
      }
    }
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UntagPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
