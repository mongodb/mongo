/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/OwnershipControlsRule.h>
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

OwnershipControlsRule::OwnershipControlsRule() : 
    m_objectOwnership(ObjectOwnership::NOT_SET),
    m_objectOwnershipHasBeenSet(false)
{
}

OwnershipControlsRule::OwnershipControlsRule(const XmlNode& xmlNode)
  : OwnershipControlsRule()
{
  *this = xmlNode;
}

OwnershipControlsRule& OwnershipControlsRule::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode objectOwnershipNode = resultNode.FirstChild("ObjectOwnership");
    if(!objectOwnershipNode.IsNull())
    {
      m_objectOwnership = ObjectOwnershipMapper::GetObjectOwnershipForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(objectOwnershipNode.GetText()).c_str()).c_str());
      m_objectOwnershipHasBeenSet = true;
    }
  }

  return *this;
}

void OwnershipControlsRule::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_objectOwnershipHasBeenSet)
  {
   XmlNode objectOwnershipNode = parentNode.CreateChildElement("ObjectOwnership");
   objectOwnershipNode.SetText(ObjectOwnershipMapper::GetNameForObjectOwnership(m_objectOwnership));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
