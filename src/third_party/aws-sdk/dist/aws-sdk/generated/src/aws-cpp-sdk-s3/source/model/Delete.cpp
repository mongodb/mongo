/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Delete.h>
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

Delete::Delete() : 
    m_objectsHasBeenSet(false),
    m_quiet(false),
    m_quietHasBeenSet(false)
{
}

Delete::Delete(const XmlNode& xmlNode)
  : Delete()
{
  *this = xmlNode;
}

Delete& Delete::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode objectsNode = resultNode.FirstChild("Object");
    if(!objectsNode.IsNull())
    {
      XmlNode objectMember = objectsNode;
      while(!objectMember.IsNull())
      {
        m_objects.push_back(objectMember);
        objectMember = objectMember.NextNode("Object");
      }

      m_objectsHasBeenSet = true;
    }
    XmlNode quietNode = resultNode.FirstChild("Quiet");
    if(!quietNode.IsNull())
    {
      m_quiet = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(quietNode.GetText()).c_str()).c_str());
      m_quietHasBeenSet = true;
    }
  }

  return *this;
}

void Delete::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_objectsHasBeenSet)
  {
   for(const auto& item : m_objects)
   {
     XmlNode objectsNode = parentNode.CreateChildElement("Object");
     item.AddToNode(objectsNode);
   }
  }

  if(m_quietHasBeenSet)
  {
   XmlNode quietNode = parentNode.CreateChildElement("Quiet");
   ss << std::boolalpha << m_quiet;
   quietNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
