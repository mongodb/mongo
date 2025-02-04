/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListEntitiesForPolicyRequest.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws::IAM::Model;
using namespace Aws::Utils;

ListEntitiesForPolicyRequest::ListEntitiesForPolicyRequest() : 
    m_policyArnHasBeenSet(false),
    m_entityFilter(EntityType::NOT_SET),
    m_entityFilterHasBeenSet(false),
    m_pathPrefixHasBeenSet(false),
    m_policyUsageFilter(PolicyUsageType::NOT_SET),
    m_policyUsageFilterHasBeenSet(false),
    m_markerHasBeenSet(false),
    m_maxItems(0),
    m_maxItemsHasBeenSet(false)
{
}

Aws::String ListEntitiesForPolicyRequest::SerializePayload() const
{
  Aws::StringStream ss;
  ss << "Action=ListEntitiesForPolicy&";
  if(m_policyArnHasBeenSet)
  {
    ss << "PolicyArn=" << StringUtils::URLEncode(m_policyArn.c_str()) << "&";
  }

  if(m_entityFilterHasBeenSet)
  {
    ss << "EntityFilter=" << EntityTypeMapper::GetNameForEntityType(m_entityFilter) << "&";
  }

  if(m_pathPrefixHasBeenSet)
  {
    ss << "PathPrefix=" << StringUtils::URLEncode(m_pathPrefix.c_str()) << "&";
  }

  if(m_policyUsageFilterHasBeenSet)
  {
    ss << "PolicyUsageFilter=" << PolicyUsageTypeMapper::GetNameForPolicyUsageType(m_policyUsageFilter) << "&";
  }

  if(m_markerHasBeenSet)
  {
    ss << "Marker=" << StringUtils::URLEncode(m_marker.c_str()) << "&";
  }

  if(m_maxItemsHasBeenSet)
  {
    ss << "MaxItems=" << m_maxItems << "&";
  }

  ss << "Version=2010-05-08";
  return ss.str();
}


void  ListEntitiesForPolicyRequest::DumpBodyToUrl(Aws::Http::URI& uri ) const
{
  uri.SetQueryString(SerializePayload());
}
