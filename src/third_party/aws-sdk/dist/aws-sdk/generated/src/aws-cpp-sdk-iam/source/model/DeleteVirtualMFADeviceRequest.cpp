/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteVirtualMFADeviceRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

DeleteVirtualMFADeviceRequest::DeleteVirtualMFADeviceRequest() : 
    m_serialNumberHasBeenSet(false)
{
}

Aws::String DeleteVirtualMFADeviceRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=DeleteVirtualMFADevice&";
  if(m_serialNumberHasBeenSet)
  {
    ss << "SerialNumber=" << StringUtils::URLEncode(m_serialNumber.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  DeleteVirtualMFADeviceRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
