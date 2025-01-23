/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListSAMLProvidersRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

ListSAMLProvidersRequest::ListSAMLProvidersRequest()
{
}

Aws::String ListSAMLProvidersRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=ListSAMLProviders&";
  ss << "Version=2010-05-08";
  return ss.str();
}


void  ListSAMLProvidersRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
