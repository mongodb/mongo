/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetAccountPasswordPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetAccountPasswordPolicyRequest::GetAccountPasswordPolicyRequest()
{
}

Aws::String GetAccountPasswordPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetAccountPasswordPolicy&";
  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetAccountPasswordPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
