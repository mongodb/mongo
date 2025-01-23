/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetSSHPublicKeyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetSSHPublicKeyRequest::GetSSHPublicKeyRequest() : 
    m_userNameHasBeenSet(false),
    m_sSHPublicKeyIdHasBeenSet(false),
    m_encoding(EncodingType::NOT_SET),
    m_encodingHasBeenSet(false)
{
}

Aws::String GetSSHPublicKeyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetSSHPublicKey&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_sSHPublicKeyIdHasBeenSet)
  {
    ss << "SSHPublicKeyId=" << StringUtils::URLEncode(m_sSHPublicKeyId.c_str()) << "&";
  }

  if(m_encodingHasBeenSet)
  {
    ss << "Encoding=" << EncodingTypeMapper::GetNameForEncodingType(m_encoding) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetSSHPublicKeyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
