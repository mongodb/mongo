/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/RemoveClientIDFromOpenIDConnectProviderRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

RemoveClientIDFromOpenIDConnectProviderRequest::RemoveClientIDFromOpenIDConnectProviderRequest() : 
    m_openIDConnectProviderArnHasBeenSet(false),
    m_clientIDHasBeenSet(false)
{
}

Aws::String RemoveClientIDFromOpenIDConnectProviderRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=RemoveClientIDFromOpenIDConnectProvider&";
  if(m_openIDConnectProviderArnHasBeenSet)
  {
    ss << "OpenIDConnectProviderArn=" << StringUtils::URLEncode(m_openIDConnectProviderArn.c_str()) << "&";
  }

  if(m_clientIDHasBeenSet)
  {
    ss << "ClientID=" << StringUtils::URLEncode(m_clientID.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  RemoveClientIDFromOpenIDConnectProviderRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
