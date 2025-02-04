/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UploadSSHPublicKeyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UploadSSHPublicKeyRequest::UploadSSHPublicKeyRequest() : 
    m_userNameHasBeenSet(false),
    m_sSHPublicKeyBodyHasBeenSet(false)
{
}

Aws::String UploadSSHPublicKeyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UploadSSHPublicKey&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_sSHPublicKeyBodyHasBeenSet)
  {
    ss << "SSHPublicKeyBody=" << StringUtils::URLEncode(m_sSHPublicKeyBody.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UploadSSHPublicKeyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
