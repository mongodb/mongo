/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UpdateSigningCertificateRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UpdateSigningCertificateRequest::UpdateSigningCertificateRequest() : 
    m_userNameHasBeenSet(false),
    m_certificateIdHasBeenSet(false),
    m_status(StatusType::NOT_SET),
    m_statusHasBeenSet(false)
{
}

Aws::String UpdateSigningCertificateRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UpdateSigningCertificate&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_certificateIdHasBeenSet)
  {
    ss << "CertificateId=" << StringUtils::URLEncode(m_certificateId.c_str()) << "&";
  }

  if(m_statusHasBeenSet)
  {
    ss << "Status=" << StatusTypeMapper::GetNameForStatusType(m_status) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UpdateSigningCertificateRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
