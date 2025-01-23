/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ReplicationRule.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace S3
{
namespace Model
{

ReplicationRule::ReplicationRule() : 
    m_iDHasBeenSet(false),
    m_priority(0),
    m_priorityHasBeenSet(false),
    m_filterHasBeenSet(false),
    m_status(ReplicationRuleStatus::NOT_SET),
    m_statusHasBeenSet(false),
    m_sourceSelectionCriteriaHasBeenSet(false),
    m_existingObjectReplicationHasBeenSet(false),
    m_destinationHasBeenSet(false),
    m_deleteMarkerReplicationHasBeenSet(false)
{
}

ReplicationRule::ReplicationRule(const XmlNode& xmlNode)
  : ReplicationRule()
{
  *this = xmlNode;
}

ReplicationRule& ReplicationRule::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode iDNode = resultNode.FirstChild("ID");
    if(!iDNode.IsNull())
    {
      m_iD = Aws::Utils::Xml::DecodeEscapedXmlText(iDNode.GetText());
      m_iDHasBeenSet = true;
    }
    XmlNode priorityNode = resultNode.FirstChild("Priority");
    if(!priorityNode.IsNull())
    {
      m_priority = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(priorityNode.GetText()).c_str()).c_str());
      m_priorityHasBeenSet = true;
    }
    XmlNode filterNode = resultNode.FirstChild("Filter");
    if(!filterNode.IsNull())
    {
      m_filter = filterNode;
      m_filterHasBeenSet = true;
    }
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = ReplicationRuleStatusMapper::GetReplicationRuleStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
      m_statusHasBeenSet = true;
    }
    XmlNode sourceSelectionCriteriaNode = resultNode.FirstChild("SourceSelectionCriteria");
    if(!sourceSelectionCriteriaNode.IsNull())
    {
      m_sourceSelectionCriteria = sourceSelectionCriteriaNode;
      m_sourceSelectionCriteriaHasBeenSet = true;
    }
    XmlNode existingObjectReplicationNode = resultNode.FirstChild("ExistingObjectReplication");
    if(!existingObjectReplicationNode.IsNull())
    {
      m_existingObjectReplication = existingObjectReplicationNode;
      m_existingObjectReplicationHasBeenSet = true;
    }
    XmlNode destinationNode = resultNode.FirstChild("Destination");
    if(!destinationNode.IsNull())
    {
      m_destination = destinationNode;
      m_destinationHasBeenSet = true;
    }
    XmlNode deleteMarkerReplicationNode = resultNode.FirstChild("DeleteMarkerReplication");
    if(!deleteMarkerReplicationNode.IsNull())
    {
      m_deleteMarkerReplication = deleteMarkerReplicationNode;
      m_deleteMarkerReplicationHasBeenSet = true;
    }
  }

  return *this;
}

void ReplicationRule::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_iDHasBeenSet)
  {
   XmlNode iDNode = parentNode.CreateChildElement("ID");
   iDNode.SetText(m_iD);
  }

  if(m_priorityHasBeenSet)
  {
   XmlNode priorityNode = parentNode.CreateChildElement("Priority");
   ss << m_priority;
   priorityNode.SetText(ss.str());
   ss.str("");
  }

  if(m_filterHasBeenSet)
  {
   XmlNode filterNode = parentNode.CreateChildElement("Filter");
   m_filter.AddToNode(filterNode);
  }

  if(m_statusHasBeenSet)
  {
   XmlNode statusNode = parentNode.CreateChildElement("Status");
   statusNode.SetText(ReplicationRuleStatusMapper::GetNameForReplicationRuleStatus(m_status));
  }

  if(m_sourceSelectionCriteriaHasBeenSet)
  {
   XmlNode sourceSelectionCriteriaNode = parentNode.CreateChildElement("SourceSelectionCriteria");
   m_sourceSelectionCriteria.AddToNode(sourceSelectionCriteriaNode);
  }

  if(m_existingObjectReplicationHasBeenSet)
  {
   XmlNode existingObjectReplicationNode = parentNode.CreateChildElement("ExistingObjectReplication");
   m_existingObjectReplication.AddToNode(existingObjectReplicationNode);
  }

  if(m_destinationHasBeenSet)
  {
   XmlNode destinationNode = parentNode.CreateChildElement("Destination");
   m_destination.AddToNode(destinationNode);
  }

  if(m_deleteMarkerReplicationHasBeenSet)
  {
   XmlNode deleteMarkerReplicationNode = parentNode.CreateChildElement("DeleteMarkerReplication");
   m_deleteMarkerReplication.AddToNode(deleteMarkerReplicationNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
