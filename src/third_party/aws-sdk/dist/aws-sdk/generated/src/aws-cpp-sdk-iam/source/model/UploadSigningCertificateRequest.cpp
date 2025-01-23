/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UploadSigningCertificateRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UploadSigningCertificateRequest::UploadSigningCertificateRequest() : 
    m_userNameHasBeenSet(false),
    m_certificateBodyHasBeenSet(false)
{
}

Aws::String UploadSigningCertificateRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UploadSigningCertificate&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_certificateBodyHasBeenSet)
  {
    ss << "CertificateBody=" << StringUtils::URLEncode(m_certificateBody.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UploadSigningCertificateRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
