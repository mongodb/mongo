/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/EnableMFADeviceRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

EnableMFADeviceRequest::EnableMFADeviceRequest() : 
    m_userNameHasBeenSet(false),
    m_serialNumberHasBeenSet(false),
    m_authenticationCode1HasBeenSet(false),
    m_authenticationCode2HasBeenSet(false)
{
}

Aws::String EnableMFADeviceRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=EnableMFADevice&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_serialNumberHasBeenSet)
  {
    ss << "SerialNumber=" << StringUtils::URLEncode(m_serialNumber.c_str()) << "&";
  }

  if(m_authenticationCode1HasBeenSet)
  {
    ss << "AuthenticationCode1=" << StringUtils::URLEncode(m_authenticationCode1.c_str()) << "&";
  }

  if(m_authenticationCode2HasBeenSet)
  {
    ss << "AuthenticationCode2=" << StringUtils::URLEncode(m_authenticationCode2.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  EnableMFADeviceRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
