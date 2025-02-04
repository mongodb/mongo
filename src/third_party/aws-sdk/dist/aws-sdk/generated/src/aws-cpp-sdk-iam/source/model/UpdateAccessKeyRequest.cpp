/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/UpdateAccessKeyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

UpdateAccessKeyRequest::UpdateAccessKeyRequest() : 
    m_userNameHasBeenSet(false),
    m_accessKeyIdHasBeenSet(false),
    m_status(StatusType::NOT_SET),
    m_statusHasBeenSet(false)
{
}

Aws::String UpdateAccessKeyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=UpdateAccessKey&";
  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_accessKeyIdHasBeenSet)
  {
    ss << "AccessKeyId=" << StringUtils::URLEncode(m_accessKeyId.c_str()) << "&";
  }

  if(m_statusHasBeenSet)
  {
    ss << "Status=" << StatusTypeMapper::GetNameForStatusType(m_status) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  UpdateAccessKeyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
