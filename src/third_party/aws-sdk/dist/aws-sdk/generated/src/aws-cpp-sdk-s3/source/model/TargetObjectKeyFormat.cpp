/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/TargetObjectKeyFormat.h>
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

TargetObjectKeyFormat::TargetObjectKeyFormat() : 
    m_simplePrefixHasBeenSet(false),
    m_partitionedPrefixHasBeenSet(false)
{
}

TargetObjectKeyFormat::TargetObjectKeyFormat(const XmlNode& xmlNode)
  : TargetObjectKeyFormat()
{
  *this = xmlNode;
}

TargetObjectKeyFormat& TargetObjectKeyFormat::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode simplePrefixNode = resultNode.FirstChild("SimplePrefix");
    if(!simplePrefixNode.IsNull())
    {
      m_simplePrefix = simplePrefixNode;
      m_simplePrefixHasBeenSet = true;
    }
    XmlNode partitionedPrefixNode = resultNode.FirstChild("PartitionedPrefix");
    if(!partitionedPrefixNode.IsNull())
    {
      m_partitionedPrefix = partitionedPrefixNode;
      m_partitionedPrefixHasBeenSet = true;
    }
  }

  return *this;
}

void TargetObjectKeyFormat::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_simplePrefixHasBeenSet)
  {
   XmlNode simplePrefixNode = parentNode.CreateChildElement("SimplePrefix");
   m_simplePrefix.AddToNode(simplePrefixNode);
  }

  if(m_partitionedPrefixHasBeenSet)
  {
   XmlNode partitionedPrefixNode = parentNode.CreateChildElement("PartitionedPrefix");
   m_partitionedPrefix.AddToNode(partitionedPrefixNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
