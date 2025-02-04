/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Grantee.h>
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

Grantee::Grantee() : 
    m_displayNameHasBeenSet(false),
    m_emailAddressHasBeenSet(false),
    m_iDHasBeenSet(false),
    m_type(Type::NOT_SET),
    m_typeHasBeenSet(false),
    m_uRIHasBeenSet(false)
{
}

Grantee::Grantee(const XmlNode& xmlNode)
  : Grantee()
{
  *this = xmlNode;
}

Grantee& Grantee::operator =(const XmlNode& xmlNode)
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
    XmlNode emailAddressNode = resultNode.FirstChild("EmailAddress");
    if(!emailAddressNode.IsNull())
    {
      m_emailAddress = Aws::Utils::Xml::DecodeEscapedXmlText(emailAddressNode.GetText());
      m_emailAddressHasBeenSet = true;
    }
    XmlNode iDNode = resultNode.FirstChild("ID");
    if(!iDNode.IsNull())
    {
      m_iD = Aws::Utils::Xml::DecodeEscapedXmlText(iDNode.GetText());
      m_iDHasBeenSet = true;
    }
    auto type = resultNode.GetAttributeValue("xsi:type");
    if(!type.empty())
    {
      m_type = TypeMapper::GetTypeForName(StringUtils::Trim(type.c_str()).c_str());
      m_typeHasBeenSet = true;
    }
    XmlNode uRINode = resultNode.FirstChild("URI");
    if(!uRINode.IsNull())
    {
      m_uRI = Aws::Utils::Xml::DecodeEscapedXmlText(uRINode.GetText());
      m_uRIHasBeenSet = true;
    }
  }

  return *this;
}

void Grantee::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  parentNode.SetAttributeValue("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
  if(m_displayNameHasBeenSet)
  {
   XmlNode displayNameNode = parentNode.CreateChildElement("DisplayName");
   displayNameNode.SetText(m_displayName);
  }

  if(m_emailAddressHasBeenSet)
  {
   XmlNode emailAddressNode = parentNode.CreateChildElement("EmailAddress");
   emailAddressNode.SetText(m_emailAddress);
  }

  if(m_iDHasBeenSet)
  {
   XmlNode iDNode = parentNode.CreateChildElement("ID");
   iDNode.SetText(m_iD);
  }

  if(m_typeHasBeenSet)
  {
   parentNode.SetAttributeValue("xsi:type", TypeMapper::GetNameForType(m_type));
  }

  if(m_uRIHasBeenSet)
  {
   XmlNode uRINode = parentNode.CreateChildElement("URI");
   uRINode.SetText(m_uRI);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
