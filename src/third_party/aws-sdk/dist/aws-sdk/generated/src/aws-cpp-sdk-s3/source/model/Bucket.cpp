/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Bucket.h>
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

Bucket::Bucket() : 
    m_nameHasBeenSet(false),
    m_creationDateHasBeenSet(false),
    m_bucketRegionHasBeenSet(false)
{
}

Bucket::Bucket(const XmlNode& xmlNode)
  : Bucket()
{
  *this = xmlNode;
}

Bucket& Bucket::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode nameNode = resultNode.FirstChild("Name");
    if(!nameNode.IsNull())
    {
      m_name = Aws::Utils::Xml::DecodeEscapedXmlText(nameNode.GetText());
      m_nameHasBeenSet = true;
    }
    XmlNode creationDateNode = resultNode.FirstChild("CreationDate");
    if(!creationDateNode.IsNull())
    {
      m_creationDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(creationDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_creationDateHasBeenSet = true;
    }
    XmlNode bucketRegionNode = resultNode.FirstChild("BucketRegion");
    if(!bucketRegionNode.IsNull())
    {
      m_bucketRegion = Aws::Utils::Xml::DecodeEscapedXmlText(bucketRegionNode.GetText());
      m_bucketRegionHasBeenSet = true;
    }
  }

  return *this;
}

void Bucket::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_nameHasBeenSet)
  {
   XmlNode nameNode = parentNode.CreateChildElement("Name");
   nameNode.SetText(m_name);
  }

  if(m_creationDateHasBeenSet)
  {
   XmlNode creationDateNode = parentNode.CreateChildElement("CreationDate");
   creationDateNode.SetText(m_creationDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

  if(m_bucketRegionHasBeenSet)
  {
   XmlNode bucketRegionNode = parentNode.CreateChildElement("BucketRegion");
   bucketRegionNode.SetText(m_bucketRegion);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
