/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ChangePasswordRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

ChangePasswordRequest::ChangePasswordRequest() : 
    m_oldPasswordHasBeenSet(false),
    m_newPasswordHasBeenSet(false)
{
}

Aws::String ChangePasswordRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=ChangePassword&";
  if(m_oldPasswordHasBeenSet)
  {
    ss << "OldPassword=" << StringUtils::URLEncode(m_oldPassword.c_str()) << "&";
  }

  if(m_newPasswordHasBeenSet)
  {
    ss << "NewPassword=" << StringUtils::URLEncode(m_newPassword.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  ChangePasswordRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
