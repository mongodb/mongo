/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UploadServerCertificateRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UploadServerCertificateRequest::UploadServerCertificateRequest() : 
    m_pathHasBeenSet(false),
    m_serverCertificateNameHasBeenSet(false),
    m_certificateBodyHasBeenSet(false),
    m_privateKeyHasBeenSet(false),
    m_certificateChainHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String UploadServerCertificateRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UploadServerCertificate&";
  if(m_pathHasBeenSet)
  {
    ss << "Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_serverCertificateNameHasBeenSet)
  {
    ss << "ServerCertificateName=" << StringUtils::URLEncode(m_serverCertificateName.c_str()) << "&";
  }

  if(m_certificateBodyHasBeenSet)
  {
    ss << "CertificateBody=" << StringUtils::URLEncode(m_certificateBody.c_str()) << "&";
  }

  if(m_privateKeyHasBeenSet)
  {
    ss << "PrivateKey=" << StringUtils::URLEncode(m_privateKey.c_str()) << "&";
  }

  if(m_certificateChainHasBeenSet)
  {
    ss << "CertificateChain=" << StringUtils::URLEncode(m_certificateChain.c_str()) << "&";
  }

  if(m_tagsHasBeenSet)
  {
    if (m_tags.empty())
    {
      ss << "Tags=&";
    }
    else
    {
      unsigned tagsCount = 1;
      for(auto& item : m_tags)
      {
        item.OutputToStream(ss, "Tags.member.", tagsCount, "");
        tagsCount++;
      }
    }
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UploadServerCertificateRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
