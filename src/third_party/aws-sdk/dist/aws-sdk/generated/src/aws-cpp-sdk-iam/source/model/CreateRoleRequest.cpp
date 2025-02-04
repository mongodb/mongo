/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateRoleRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateRoleRequest::CreateRoleRequest() : 
    m_pathHasBeenSet(false),
    m_roleNameHasBeenSet(false),
    m_assumeRolePolicyDocumentHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_maxSessionDuration(0),
    m_maxSessionDurationHasBeenSet(false),
    m_permissionsBoundaryHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String CreateRoleRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateRole&";
  if(m_pathHasBeenSet)
  {
    ss << "Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_roleNameHasBeenSet)
  {
    ss << "RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  if(m_assumeRolePolicyDocumentHasBeenSet)
  {
    ss << "AssumeRolePolicyDocument=" << StringUtils::URLEncode(m_assumeRolePolicyDocument.c_str()) << "&";
  }

  if(m_descriptionHasBeenSet)
  {
    ss << "Description=" << StringUtils::URLEncode(m_description.c_str()) << "&";
  }

  if(m_maxSessionDurationHasBeenSet)
  {
    ss << "MaxSessionDuration=" << m_maxSessionDuration << "&";
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


void  CreateRoleRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
