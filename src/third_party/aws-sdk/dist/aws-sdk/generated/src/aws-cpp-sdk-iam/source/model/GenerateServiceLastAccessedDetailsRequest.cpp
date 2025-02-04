/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GenerateServiceLastAccessedDetailsRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

GenerateServiceLastAccessedDetailsRequest::GenerateServiceLastAccessedDetailsRequest() : 
    m_arnHasBeenSet(false),
    m_granularity(AccessAdvisorUsageGranularityType::NOT_SET),
    m_granularityHasBeenSet(false)
{
}

Aws::String GenerateServiceLastAccessedDetailsRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=GenerateServiceLastAccessedDetails&";
  if(m_arnHasBeenSet)
  {
    ss << "Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }

  if(m_granularityHasBeenSet)
  {
    ss << "Granularity=" << AccessAdvisorUsageGranularityTypeMapper::GetNameForAccessAdvisorUsageGranularityType(m_granularity) << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  GenerateServiceLastAccessedDetailsRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
