/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateServiceLinkedRoleRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

CreateServiceLinkedRoleRequest::CreateServiceLinkedRoleRequest() : 
    m_aWSServiceNameHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_customSuffixHasBeenSet(false)
{
}

Aws::String CreateServiceLinkedRoleRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=CreateServiceLinkedRole&";
  if(m_aWSServiceNameHasBeenSet)
  {
    ss << "AWSServiceName=" << StringUtils::URLEncode(m_aWSServiceName.c_str()) << "&";
  }

  if(m_descriptionHasBeenSet)
  {
    ss << "Description=" << StringUtils::URLEncode(m_description.c_str()) << "&";
  }

  if(m_customSuffixHasBeenSet)
  {
    ss << "CustomSuffix=" << StringUtils::URLEncode(m_customSuffix.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  CreateServiceLinkedRoleRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
