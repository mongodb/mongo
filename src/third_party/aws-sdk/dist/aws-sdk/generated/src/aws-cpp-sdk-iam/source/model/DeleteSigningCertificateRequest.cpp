/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteSigningCertificateRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteSigningCertificateRequest::DeleteSigningCertificateRequest() : 
    m_userNameHasBeenSet(false),
    m_certificateIdHasBeenSet(false)
{
}

Aws::String DeleteSigningCertificateRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteSigningCertificate&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_certificateIdHasBeenSet)
  {
    ss << "CertificateId=" << StringUtils::URLEncode(m_certificateId.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteSigningCertificateRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
