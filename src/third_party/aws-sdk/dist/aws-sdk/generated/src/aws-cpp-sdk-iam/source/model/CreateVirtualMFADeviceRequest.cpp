/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateVirtualMFADeviceRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateVirtualMFADeviceRequest::CreateVirtualMFADeviceRequest() : 
    m_pathHasBeenSet(false),
    m_virtualMFADeviceNameHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String CreateVirtualMFADeviceRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateVirtualMFADevice&";
  if(m_pathHasBeenSet)
  {
    ss << "Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_virtualMFADeviceNameHasBeenSet)
  {
    ss << "VirtualMFADeviceName=" << StringUtils::URLEncode(m_virtualMFADeviceName.c_str()) << "&";
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


void  CreateVirtualMFADeviceRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
