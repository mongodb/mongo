/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/InputSerialization.h>
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

InputSerialization::InputSerialization() : 
    m_cSVHasBeenSet(false),
    m_compressionType(CompressionType::NOT_SET),
    m_compressionTypeHasBeenSet(false),
    m_jSONHasBeenSet(false),
    m_parquetHasBeenSet(false)
{
}

InputSerialization::InputSerialization(const XmlNode& xmlNode)
  : InputSerialization()
{
  *this = xmlNode;
}

InputSerialization& InputSerialization::operator =(const XmlNode& xmlNode)
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
    XmlNode compressionTypeNode = resultNode.FirstChild("CompressionType");
    if(!compressionTypeNode.IsNull())
    {
      m_compressionType = CompressionTypeMapper::GetCompressionTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(compressionTypeNode.GetText()).c_str()).c_str());
      m_compressionTypeHasBeenSet = true;
    }
    XmlNode jSONNode = resultNode.FirstChild("JSON");
    if(!jSONNode.IsNull())
    {
      m_jSON = jSONNode;
      m_jSONHasBeenSet = true;
    }
    XmlNode parquetNode = resultNode.FirstChild("Parquet");
    if(!parquetNode.IsNull())
    {
      m_parquet = parquetNode;
      m_parquetHasBeenSet = true;
    }
  }

  return *this;
}

void InputSerialization::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_cSVHasBeenSet)
  {
   XmlNode cSVNode = parentNode.CreateChildElement("CSV");
   m_cSV.AddToNode(cSVNode);
  }

  if(m_compressionTypeHasBeenSet)
  {
   XmlNode compressionTypeNode = parentNode.CreateChildElement("CompressionType");
   compressionTypeNode.SetText(CompressionTypeMapper::GetNameForCompressionType(m_compressionType));
  }

  if(m_jSONHasBeenSet)
  {
   XmlNode jSONNode = parentNode.CreateChildElement("JSON");
   m_jSON.AddToNode(jSONNode);
  }

  if(m_parquetHasBeenSet)
  {
   XmlNode parquetNode = parentNode.CreateChildElement("Parquet");
   m_parquet.AddToNode(parquetNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
