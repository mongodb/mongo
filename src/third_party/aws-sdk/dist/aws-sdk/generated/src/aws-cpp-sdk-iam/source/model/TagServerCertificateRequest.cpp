/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/TagServerCertificateRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

TagServerCertificateRequest::TagServerCertificateRequest() : 
    m_serverCertificateNameHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String TagServerCertificateRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=TagServerCertificate&";
  if(m_serverCertificateNameHasBeenSet)
  {
    ss << "ServerCertificateName=" << StringUtils::URLEncode(m_serverCertificateName.c_str()) << "&";
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


void  TagServerCertificateRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
