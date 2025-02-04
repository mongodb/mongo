/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/PartitionedPrefix.h>
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

PartitionedPrefix::PartitionedPrefix() : 
    m_partitionDateSource(PartitionDateSource::NOT_SET),
    m_partitionDateSourceHasBeenSet(false)
{
}

PartitionedPrefix::PartitionedPrefix(const XmlNode& xmlNode)
  : PartitionedPrefix()
{
  *this = xmlNode;
}

PartitionedPrefix& PartitionedPrefix::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode partitionDateSourceNode = resultNode.FirstChild("PartitionDateSource");
    if(!partitionDateSourceNode.IsNull())
    {
      m_partitionDateSource = PartitionDateSourceMapper::GetPartitionDateSourceForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(partitionDateSourceNode.GetText()).c_str()).c_str());
      m_partitionDateSourceHasBeenSet = true;
    }
  }

  return *this;
}

void PartitionedPrefix::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_partitionDateSourceHasBeenSet)
  {
   XmlNode partitionDateSourceNode = parentNode.CreateChildElement("PartitionDateSource");
   partitionDateSourceNode.SetText(PartitionDateSourceMapper::GetNameForPartitionDateSource(m_partitionDateSource));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
