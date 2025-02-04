/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UpdateServerCertificateRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UpdateServerCertificateRequest::UpdateServerCertificateRequest() : 
    m_serverCertificateNameHasBeenSet(false),
    m_newPathHasBeenSet(false),
    m_newServerCertificateNameHasBeenSet(false)
{
}

Aws::String UpdateServerCertificateRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UpdateServerCertificate&";
  if(m_serverCertificateNameHasBeenSet)
  {
    ss << "ServerCertificateName=" << StringUtils::URLEncode(m_serverCertificateName.c_str()) << "&";
  }

  if(m_newPathHasBeenSet)
  {
    ss << "NewPath=" << StringUtils::URLEncode(m_newPath.c_str()) << "&";
  }

  if(m_newServerCertificateNameHasBeenSet)
  {
    ss << "NewServerCertificateName=" << StringUtils::URLEncode(m_newServerCertificateName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UpdateServerCertificateRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
