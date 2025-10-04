/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/TagSAMLProviderRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

TagSAMLProviderRequest::TagSAMLProviderRequest() : 
    m_sAMLProviderArnHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String TagSAMLProviderRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=TagSAMLProvider&";
  if(m_sAMLProviderArnHasBeenSet)
  {
    ss << "SAMLProviderArn=" << StringUtils::URLEncode(m_sAMLProviderArn.c_str()) << "&";
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


void  TagSAMLProviderRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
