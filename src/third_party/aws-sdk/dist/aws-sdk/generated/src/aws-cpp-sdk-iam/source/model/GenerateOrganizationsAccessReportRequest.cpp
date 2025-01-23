/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GenerateOrganizationsAccessReportRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GenerateOrganizationsAccessReportRequest::GenerateOrganizationsAccessReportRequest() : 
    m_entityPathHasBeenSet(false),
    m_organizationsPolicyIdHasBeenSet(false)
{
}

Aws::String GenerateOrganizationsAccessReportRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GenerateOrganizationsAccessReport&";
  if(m_entityPathHasBeenSet)
  {
    ss << "EntityPath=" << StringUtils::URLEncode(m_entityPath.c_str()) << "&";
  }

  if(m_organizationsPolicyIdHasBeenSet)
  {
    ss << "OrganizationsPolicyId=" << StringUtils::URLEncode(m_organizationsPolicyId.c_str()) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GenerateOrganizationsAccessReportRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
