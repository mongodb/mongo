/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateUserRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateUserRequest::CreateUserRequest() : 
    m_pathHasBeenSet(false),
    m_userNameHasBeenSet(false),
    m_permissionsBoundaryHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String CreateUserRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateUser&";
  if(m_pathHasBeenSet)
  {
    ss << "Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_userNameHasBeenSet)
  {
    ss << "UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_permissionsBoundaryHasBeenSet)
  {
    ss << "PermissionsBoundary=" << StringUtils::URLEncode(m_permissionsBoundary.c_str()) << "&";
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


void  CreateUserRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
