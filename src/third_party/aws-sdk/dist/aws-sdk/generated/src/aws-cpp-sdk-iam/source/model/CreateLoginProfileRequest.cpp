/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateLoginProfileRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateLoginProfileRequest::CreateLoginProfileRequest() : 
    m_userNameHasBeenSet(false),
    m_passwordHasBeenSet(false),
    m_passwordResetRequired(false),
    m_passwordResetRequiredHasBeenSet(false)
{
}

Aws::String CreateLoginProfileRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateLoginProfile&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_passwordHasBeenSet)
  {
    ss << "Password=" << StringUtils::URLEncode(m_password.c_str()) << "&";
  }

  if(m_passwordResetRequiredHasBeenSet)
  {
    ss << "PasswordResetRequired=" << std::boolalpha << m_passwordResetRequired << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  CreateLoginProfileRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
