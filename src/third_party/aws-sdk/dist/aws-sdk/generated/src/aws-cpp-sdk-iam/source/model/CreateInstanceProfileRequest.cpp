/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateInstanceProfileRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateInstanceProfileRequest::CreateInstanceProfileRequest() : 
    m_instanceProfileNameHasBeenSet(false),
    m_pathHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String CreateInstanceProfileRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateInstanceProfile&";
  if(m_instanceProfileNameHasBeenSet)
  {
    ss << "InstanceProfileName=" << StringUtils::URLEncode(m_instanceProfileName.c_str()) << "&";
  }

  if(m_pathHasBeenSet)
  {
    ss << "Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
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


void  CreateInstanceProfileRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
