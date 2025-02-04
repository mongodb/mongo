/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetAccessKeyLastUsedRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GetAccessKeyLastUsedRequest::GetAccessKeyLastUsedRequest() : 
    m_accessKeyIdHasBeenSet(false)
{
}

Aws::String GetAccessKeyLastUsedRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GetAccessKeyLastUsed&";
  if(m_accessKeyIdHasBeenSet)
  {
    ss << "AccessKeyId=" << StringUtils::URLEncode(m_accessKeyId.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GetAccessKeyLastUsedRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
