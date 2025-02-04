/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/SelectParameters.h>
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

SelectParameters::SelectParameters() : 
    m_inputSerializationHasBeenSet(false),
    m_expressionType(ExpressionType::NOT_SET),
    m_expressionTypeHasBeenSet(false),
    m_expressionHasBeenSet(false),
    m_outputSerializationHasBeenSet(false)
{
}

SelectParameters::SelectParameters(const XmlNode& xmlNode)
  : SelectParameters()
{
  *this = xmlNode;
}

SelectParameters& SelectParameters::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode inputSerializationNode = resultNode.FirstChild("InputSerialization");
    if(!inputSerializationNode.IsNull())
    {
      m_inputSerialization = inputSerializationNode;
      m_inputSerializationHasBeenSet = true;
    }
    XmlNode expressionTypeNode = resultNode.FirstChild("ExpressionType");
    if(!expressionTypeNode.IsNull())
    {
      m_expressionType = ExpressionTypeMapper::GetExpressionTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(expressionTypeNode.GetText()).c_str()).c_str());
      m_expressionTypeHasBeenSet = true;
    }
    XmlNode expressionNode = resultNode.FirstChild("Expression");
    if(!expressionNode.IsNull())
    {
      m_expression = Aws::Utils::Xml::DecodeEscapedXmlText(expressionNode.GetText());
      m_expressionHasBeenSet = true;
    }
    XmlNode outputSerializationNode = resultNode.FirstChild("OutputSerialization");
    if(!outputSerializationNode.IsNull())
    {
      m_outputSerialization = outputSerializationNode;
      m_outputSerializationHasBeenSet = true;
    }
  }

  return *this;
}

void SelectParameters::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_inputSerializationHasBeenSet)
  {
   XmlNode inputSerializationNode = parentNode.CreateChildElement("InputSerialization");
   m_inputSerialization.AddToNode(inputSerializationNode);
  }

  if(m_expressionTypeHasBeenSet)
  {
   XmlNode expressionTypeNode = parentNode.CreateChildElement("ExpressionType");
   expressionTypeNode.SetText(ExpressionTypeMapper::GetNameForExpressionType(m_expressionType));
  }

  if(m_expressionHasBeenSet)
  {
   XmlNode expressionNode = parentNode.CreateChildElement("Expression");
   expressionNode.SetText(m_expression);
  }

  if(m_outputSerializationHasBeenSet)
  {
   XmlNode outputSerializationNode = parentNode.CreateChildElement("OutputSerialization");
   m_outputSerialization.AddToNode(outputSerializationNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
