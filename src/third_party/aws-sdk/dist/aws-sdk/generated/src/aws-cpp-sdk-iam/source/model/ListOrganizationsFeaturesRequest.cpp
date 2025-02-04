/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListOrganizationsFeaturesRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

ListOrganizationsFeaturesRequest::ListOrganizationsFeaturesRequest()
{
}

Aws::String ListOrganizationsFeaturesRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=ListOrganizationsFeatures&";
  ss << "Version=2010-05-08";
  return ss.str();
}


void  ListOrganizationsFeaturesRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
