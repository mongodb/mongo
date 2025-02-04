/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteInstanceProfileRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteInstanceProfileRequest::DeleteInstanceProfileRequest() : 
    m_instanceProfileNameHasBeenSet(false)
{
}

Aws::String DeleteInstanceProfileRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteInstanceProfile&";
  if(m_instanceProfileNameHasBeenSet)
  {
    ss << "InstanceProfileName=" << StringUtils::URLEncode(m_instanceProfileName.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteInstanceProfileRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
