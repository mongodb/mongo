/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ResetServiceSpecificCredentialRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

ResetServiceSpecificCredentialRequest::ResetServiceSpecificCredentialRequest() : 
    m_userNameHasBeenSet(false),
    m_serviceSpecificCredentialIdHasBeenSet(false)
{
}

Aws::String ResetServiceSpecificCredentialRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=ResetServiceSpecificCredential&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_serviceSpecificCredentialIdHasBeenSet)
  {
    ss << "ServiceSpecificCredentialId=" << StringUtils::URLEncode(m_serviceSpecificCredentialId.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  ResetServiceSpecificCredentialRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
