/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteServiceSpecificCredentialRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteServiceSpecificCredentialRequest::DeleteServiceSpecificCredentialRequest() : 
    m_userNameHasBeenSet(false),
    m_serviceSpecificCredentialIdHasBeenSet(false)
{
}

Aws::String DeleteServiceSpecificCredentialRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteServiceSpecificCredential&";
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


void  DeleteServiceSpecificCredentialRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
