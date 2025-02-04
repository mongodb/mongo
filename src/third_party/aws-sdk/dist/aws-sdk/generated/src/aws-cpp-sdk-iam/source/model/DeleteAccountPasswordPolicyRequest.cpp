/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteAccountPasswordPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteAccountPasswordPolicyRequest::DeleteAccountPasswordPolicyRequest()
{
}

Aws::String DeleteAccountPasswordPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteAccountPasswordPolicy&";
  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteAccountPasswordPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
