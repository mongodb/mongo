/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateOpenIDConnectProviderRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateOpenIDConnectProviderRequest::CreateOpenIDConnectProviderRequest() : 
    m_urlHasBeenSet(false),
    m_clientIDListHasBeenSet(false),
    m_thumbprintListHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String CreateOpenIDConnectProviderRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateOpenIDConnectProvider&";
  if(m_urlHasBeenSet)
  {
    ss << "Url=" << StringUtils::URLEncode(m_url.c_str()) << "&";
  }

  if(m_clientIDListHasBeenSet)
  {
    if (m_clientIDList.empty())
    {
      ss << "ClientIDList=&";
    }
    else
    {
      unsigned clientIDListCount = 1;
      for(auto& item : m_clientIDList)
      {
        ss << "ClientIDList.member." << clientIDListCount << "="
            << StringUtils::URLEncode(item.c_str()) << "&";
        clientIDListCount++;
      }
    }
  }

  if(m_thumbprintListHasBeenSet)
  {
    if (m_thumbprintList.empty())
    {
      ss << "ThumbprintList=&";
    }
    else
    {
      unsigned thumbprintListCount = 1;
      for(auto& item : m_thumbprintList)
      {
        ss << "ThumbprintList.member." << thumbprintListCount << "="
            << StringUtils::URLEncode(item.c_str()) << "&";
        thumbprintListCount++;
      }
    }
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


void  CreateOpenIDConnectProviderRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
