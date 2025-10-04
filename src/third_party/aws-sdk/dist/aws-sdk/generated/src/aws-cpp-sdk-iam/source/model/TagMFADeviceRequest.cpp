/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/TagMFADeviceRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

TagMFADeviceRequest::TagMFADeviceRequest() : 
    m_serialNumberHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String TagMFADeviceRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=TagMFADevice&";
  if(m_serialNumberHasBeenSet)
  {
    ss << "SerialNumber=" << StringUtils::URLEncode(m_serialNumber.c_str()) << "&";
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


void  TagMFADeviceRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
