/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListServiceSpecificCredentialsRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

ListServiceSpecificCredentialsRequest::ListServiceSpecificCredentialsRequest() : 
    m_userNameHasBeenSet(false),
    m_serviceNameHasBeenSet(false)
{
}

Aws::String ListServiceSpecificCredentialsRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=ListServiceSpecificCredentials&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_serviceNameHasBeenSet)
  {
    ss << "ServiceName=" << StringUtils::URLEncode(m_serviceName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  ListServiceSpecificCredentialsRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
