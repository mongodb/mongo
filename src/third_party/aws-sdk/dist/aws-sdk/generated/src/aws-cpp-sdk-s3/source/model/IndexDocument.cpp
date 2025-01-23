/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/IndexDocument.h>
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

IndexDocument::IndexDocument() : 
    m_suffixHasBeenSet(false)
{
}

IndexDocument::IndexDocument(const XmlNode& xmlNode)
  : IndexDocument()
{
  *this = xmlNode;
}

IndexDocument& IndexDocument::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode suffixNode = resultNode.FirstChild("Suffix");
    if(!suffixNode.IsNull())
    {
      m_suffix = Aws::Utils::Xml::DecodeEscapedXmlText(suffixNode.GetText());
      m_suffixHasBeenSet = true;
    }
  }

  return *this;
}

void IndexDocument::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_suffixHasBeenSet)
  {
   XmlNode suffixNode = parentNode.CreateChildElement("Suffix");
   suffixNode.SetText(m_suffix);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
