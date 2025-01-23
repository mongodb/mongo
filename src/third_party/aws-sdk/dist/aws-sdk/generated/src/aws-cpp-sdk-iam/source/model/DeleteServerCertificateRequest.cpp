/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteServerCertificateRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteServerCertificateRequest::DeleteServerCertificateRequest() : 
    m_serverCertificateNameHasBeenSet(false)
{
}

Aws::String DeleteServerCertificateRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteServerCertificate&";
  if(m_serverCertificateNameHasBeenSet)
  {
    ss << "ServerCertificateName=" << StringUtils::URLEncode(m_serverCertificateName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteServerCertificateRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
