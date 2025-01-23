/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateSAMLProviderRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateSAMLProviderRequest::CreateSAMLProviderRequest() : 
    m_sAMLMetadataDocumentHasBeenSet(false),
    m_nameHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String CreateSAMLProviderRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateSAMLProvider&";
  if(m_sAMLMetadataDocumentHasBeenSet)
  {
    ss << "SAMLMetadataDocument=" << StringUtils::URLEncode(m_sAMLMetadataDocument.c_str()) << "&";
  }

  if(m_nameHasBeenSet)
  {
    ss << "Name=" << StringUtils::URLEncode(m_name.c_str()) << "&";
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


void  CreateSAMLProviderRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
