/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/CompletedMultipartUpload.h>
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

CompletedMultipartUpload::CompletedMultipartUpload() : 
    m_partsHasBeenSet(false)
{
}

CompletedMultipartUpload::CompletedMultipartUpload(const XmlNode& xmlNode)
  : CompletedMultipartUpload()
{
  *this = xmlNode;
}

CompletedMultipartUpload& CompletedMultipartUpload::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode partsNode = resultNode.FirstChild("Part");
    if(!partsNode.IsNull())
    {
      XmlNode partMember = partsNode;
      while(!partMember.IsNull())
      {
        m_parts.push_back(partMember);
        partMember = partMember.NextNode("Part");
      }

      m_partsHasBeenSet = true;
    }
  }

  return *this;
}

void CompletedMultipartUpload::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_partsHasBeenSet)
  {
   for(const auto& item : m_parts)
   {
     XmlNode partsNode = parentNode.CreateChildElement("Part");
     item.AddToNode(partsNode);
   }
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
