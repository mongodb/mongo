/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteSAMLProviderRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteSAMLProviderRequest::DeleteSAMLProviderRequest() : 
    m_sAMLProviderArnHasBeenSet(false)
{
}

Aws::String DeleteSAMLProviderRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteSAMLProvider&";
  if(m_sAMLProviderArnHasBeenSet)
  {
    ss << "SAMLProviderArn=" << StringUtils::URLEncode(m_sAMLProviderArn.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteSAMLProviderRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
