/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectIdentifier.h>
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

ObjectIdentifier::ObjectIdentifier() : 
    m_keyHasBeenSet(false),
    m_versionIdHasBeenSet(false),
    m_eTagHasBeenSet(false),
    m_lastModifiedTimeHasBeenSet(false),
    m_size(0),
    m_sizeHasBeenSet(false)
{
}

ObjectIdentifier::ObjectIdentifier(const XmlNode& xmlNode)
  : ObjectIdentifier()
{
  *this = xmlNode;
}

ObjectIdentifier& ObjectIdentifier::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode keyNode = resultNode.FirstChild("Key");
    if(!keyNode.IsNull())
    {
      m_key = Aws::Utils::Xml::DecodeEscapedXmlText(keyNode.GetText());
      m_keyHasBeenSet = true;
    }
    XmlNode versionIdNode = resultNode.FirstChild("VersionId");
    if(!versionIdNode.IsNull())
    {
      m_versionId = Aws::Utils::Xml::DecodeEscapedXmlText(versionIdNode.GetText());
      m_versionIdHasBeenSet = true;
    }
    XmlNode eTagNode = resultNode.FirstChild("ETag");
    if(!eTagNode.IsNull())
    {
      m_eTag = Aws::Utils::Xml::DecodeEscapedXmlText(eTagNode.GetText());
      m_eTagHasBeenSet = true;
    }
    XmlNode lastModifiedTimeNode = resultNode.FirstChild("LastModifiedTime");
    if(!lastModifiedTimeNode.IsNull())
    {
      m_lastModifiedTime = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(lastModifiedTimeNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::RFC822);
      m_lastModifiedTimeHasBeenSet = true;
    }
    XmlNode sizeNode = resultNode.FirstChild("Size");
    if(!sizeNode.IsNull())
    {
      m_size = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(sizeNode.GetText()).c_str()).c_str());
      m_sizeHasBeenSet = true;
    }
  }

  return *this;
}

void ObjectIdentifier::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_keyHasBeenSet)
  {
   XmlNode keyNode = parentNode.CreateChildElement("Key");
   keyNode.SetText(m_key);
  }

  if(m_versionIdHasBeenSet)
  {
   XmlNode versionIdNode = parentNode.CreateChildElement("VersionId");
   versionIdNode.SetText(m_versionId);
  }

  if(m_eTagHasBeenSet)
  {
   XmlNode eTagNode = parentNode.CreateChildElement("ETag");
   eTagNode.SetText(m_eTag);
  }

  if(m_lastModifiedTimeHasBeenSet)
  {
   XmlNode lastModifiedTimeNode = parentNode.CreateChildElement("LastModifiedTime");
   lastModifiedTimeNode.SetText(m_lastModifiedTime.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

  if(m_sizeHasBeenSet)
  {
   XmlNode sizeNode = parentNode.CreateChildElement("Size");
   ss << m_size;
   sizeNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
