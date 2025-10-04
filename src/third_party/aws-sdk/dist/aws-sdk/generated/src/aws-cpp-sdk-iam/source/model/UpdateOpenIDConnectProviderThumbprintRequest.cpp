/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UpdateOpenIDConnectProviderThumbprintRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UpdateOpenIDConnectProviderThumbprintRequest::UpdateOpenIDConnectProviderThumbprintRequest() : 
    m_openIDConnectProviderArnHasBeenSet(false),
    m_thumbprintListHasBeenSet(false)
{
}

Aws::String UpdateOpenIDConnectProviderThumbprintRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UpdateOpenIDConnectProviderThumbprint&";
  if(m_openIDConnectProviderArnHasBeenSet)
  {
    ss << "OpenIDConnectProviderArn=" << StringUtils::URLEncode(m_openIDConnectProviderArn.c_str()) << "&";
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

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UpdateOpenIDConnectProviderThumbprintRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
