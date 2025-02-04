/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/CommonPrefix.h>
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

CommonPrefix::CommonPrefix() : 
    m_prefixHasBeenSet(false)
{
}

CommonPrefix::CommonPrefix(const XmlNode& xmlNode)
  : CommonPrefix()
{
  *this = xmlNode;
}

CommonPrefix& CommonPrefix::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode prefixNode = resultNode.FirstChild("Prefix");
    if(!prefixNode.IsNull())
    {
      m_prefix = Aws::Utils::Xml::DecodeEscapedXmlText(prefixNode.GetText());
      m_prefixHasBeenSet = true;
    }
  }

  return *this;
}

void CommonPrefix::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_prefixHasBeenSet)
  {
   XmlNode prefixNode = parentNode.CreateChildElement("Prefix");
   prefixNode.SetText(m_prefix);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
