/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/AddClientIDToOpenIDConnectProviderRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

AddClientIDToOpenIDConnectProviderRequest::AddClientIDToOpenIDConnectProviderRequest() : 
    m_openIDConnectProviderArnHasBeenSet(false),
    m_clientIDHasBeenSet(false)
{
}

Aws::String AddClientIDToOpenIDConnectProviderRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=AddClientIDToOpenIDConnectProvider&";
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


void  AddClientIDToOpenIDConnectProviderRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
