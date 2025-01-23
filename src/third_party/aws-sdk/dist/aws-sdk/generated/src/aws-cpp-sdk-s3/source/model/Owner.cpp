/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Owner.h>
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

Owner::Owner() : 
    m_displayNameHasBeenSet(false),
    m_iDHasBeenSet(false)
{
}

Owner::Owner(const XmlNode& xmlNode)
  : Owner()
{
  *this = xmlNode;
}

Owner& Owner::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode displayNameNode = resultNode.FirstChild("DisplayName");
    if(!displayNameNode.IsNull())
    {
      m_displayName = Aws::Utils::Xml::DecodeEscapedXmlText(displayNameNode.GetText());
      m_displayNameHasBeenSet = true;
    }
    XmlNode iDNode = resultNode.FirstChild("ID");
    if(!iDNode.IsNull())
    {
      m_iD = Aws::Utils::Xml::DecodeEscapedXmlText(iDNode.GetText());
      m_iDHasBeenSet = true;
    }
  }

  return *this;
}

void Owner::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_displayNameHasBeenSet)
  {
   XmlNode displayNameNode = parentNode.CreateChildElement("DisplayName");
   displayNameNode.SetText(m_displayName);
  }

  if(m_iDHasBeenSet)
  {
   XmlNode iDNode = parentNode.CreateChildElement("ID");
   iDNode.SetText(m_iD);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
