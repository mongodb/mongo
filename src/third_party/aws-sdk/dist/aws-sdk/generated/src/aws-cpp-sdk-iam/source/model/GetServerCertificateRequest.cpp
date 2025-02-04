/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetServerCertificateRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetServerCertificateRequest::GetServerCertificateRequest() : 
    m_serverCertificateNameHasBeenSet(false)
{
}

Aws::String GetServerCertificateRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetServerCertificate&";
  if(m_serverCertificateNameHasBeenSet)
  {
    ss << "ServerCertificateName=" << StringUtils::URLEncode(m_serverCertificateName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetServerCertificateRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
