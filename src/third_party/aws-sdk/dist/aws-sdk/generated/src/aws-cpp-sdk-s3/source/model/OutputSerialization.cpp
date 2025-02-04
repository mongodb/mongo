/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/OutputSerialization.h>
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

OutputSerialization::OutputSerialization() : 
    m_cSVHasBeenSet(false),
    m_jSONHasBeenSet(false)
{
}

OutputSerialization::OutputSerialization(const XmlNode& xmlNode)
  : OutputSerialization()
{
  *this = xmlNode;
}

OutputSerialization& OutputSerialization::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode cSVNode = resultNode.FirstChild("CSV");
    if(!cSVNode.IsNull())
    {
      m_cSV = cSVNode;
      m_cSVHasBeenSet = true;
    }
    XmlNode jSONNode = resultNode.FirstChild("JSON");
    if(!jSONNode.IsNull())
    {
      m_jSON = jSONNode;
      m_jSONHasBeenSet = true;
    }
  }

  return *this;
}

void OutputSerialization::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_cSVHasBeenSet)
  {
   XmlNode cSVNode = parentNode.CreateChildElement("CSV");
   m_cSV.AddToNode(cSVNode);
  }

  if(m_jSONHasBeenSet)
  {
   XmlNode jSONNode = parentNode.CreateChildElement("JSON");
   m_jSON.AddToNode(jSONNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
