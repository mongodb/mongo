/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteSSHPublicKeyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteSSHPublicKeyRequest::DeleteSSHPublicKeyRequest() : 
    m_userNameHasBeenSet(false),
    m_sSHPublicKeyIdHasBeenSet(false)
{
}

Aws::String DeleteSSHPublicKeyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteSSHPublicKey&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_sSHPublicKeyIdHasBeenSet)
  {
    ss << "SSHPublicKeyId=" << StringUtils::URLEncode(m_sSHPublicKeyId.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteSSHPublicKeyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
