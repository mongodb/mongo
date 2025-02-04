/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/SetDefaultPolicyVersionRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

SetDefaultPolicyVersionRequest::SetDefaultPolicyVersionRequest() : 
    m_policyArnHasBeenSet(false),
    m_versionIdHasBeenSet(false)
{
}

Aws::String SetDefaultPolicyVersionRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=SetDefaultPolicyVersion&";
  if(m_policyArnHasBeenSet)
  {
    ss << "PolicyArn=" << StringUtils::URLEncode(m_policyArn.c_str()) << "&";
  }

  if(m_versionIdHasBeenSet)
  {
    ss << "VersionId=" << StringUtils::URLEncode(m_versionId.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  SetDefaultPolicyVersionRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
