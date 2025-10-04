/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/DeletedObject.h>
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

DeletedObject::DeletedObject() : 
    m_keyHasBeenSet(false),
    m_versionIdHasBeenSet(false),
    m_deleteMarker(false),
    m_deleteMarkerHasBeenSet(false),
    m_deleteMarkerVersionIdHasBeenSet(false)
{
}

DeletedObject::DeletedObject(const XmlNode& xmlNode)
  : DeletedObject()
{
  *this = xmlNode;
}

DeletedObject& DeletedObject::operator =(const XmlNode& xmlNode)
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
    XmlNode deleteMarkerNode = resultNode.FirstChild("DeleteMarker");
    if(!deleteMarkerNode.IsNull())
    {
      m_deleteMarker = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(deleteMarkerNode.GetText()).c_str()).c_str());
      m_deleteMarkerHasBeenSet = true;
    }
    XmlNode deleteMarkerVersionIdNode = resultNode.FirstChild("DeleteMarkerVersionId");
    if(!deleteMarkerVersionIdNode.IsNull())
    {
      m_deleteMarkerVersionId = Aws::Utils::Xml::DecodeEscapedXmlText(deleteMarkerVersionIdNode.GetText());
      m_deleteMarkerVersionIdHasBeenSet = true;
    }
  }

  return *this;
}

void DeletedObject::AddToNode(XmlNode& parentNode) const
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

  if(m_deleteMarkerHasBeenSet)
  {
   XmlNode deleteMarkerNode = parentNode.CreateChildElement("DeleteMarker");
   ss << std::boolalpha << m_deleteMarker;
   deleteMarkerNode.SetText(ss.str());
   ss.str("");
  }

  if(m_deleteMarkerVersionIdHasBeenSet)
  {
   XmlNode deleteMarkerVersionIdNode = parentNode.CreateChildElement("DeleteMarkerVersionId");
   deleteMarkerVersionIdNode.SetText(m_deleteMarkerVersionId);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
