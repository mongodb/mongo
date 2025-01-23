/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/PolicyStatus.h>
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

PolicyStatus::PolicyStatus() : 
    m_isPublic(false),
    m_isPublicHasBeenSet(false)
{
}

PolicyStatus::PolicyStatus(const XmlNode& xmlNode)
  : PolicyStatus()
{
  *this = xmlNode;
}

PolicyStatus& PolicyStatus::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode isPublicNode = resultNode.FirstChild("IsPublic");
    if(!isPublicNode.IsNull())
    {
      m_isPublic = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(isPublicNode.GetText()).c_str()).c_str());
      m_isPublicHasBeenSet = true;
    }
  }

  return *this;
}

void PolicyStatus::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_isPublicHasBeenSet)
  {
   XmlNode isPublicNode = parentNode.CreateChildElement("IsPublic");
   ss << std::boolalpha << m_isPublic;
   isPublicNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
