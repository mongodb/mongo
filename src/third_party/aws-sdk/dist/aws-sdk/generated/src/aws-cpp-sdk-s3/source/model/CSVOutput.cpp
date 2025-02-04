/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/CSVOutput.h>
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

CSVOutput::CSVOutput() : 
    m_quoteFields(QuoteFields::NOT_SET),
    m_quoteFieldsHasBeenSet(false),
    m_quoteEscapeCharacterHasBeenSet(false),
    m_recordDelimiterHasBeenSet(false),
    m_fieldDelimiterHasBeenSet(false),
    m_quoteCharacterHasBeenSet(false)
{
}

CSVOutput::CSVOutput(const XmlNode& xmlNode)
  : CSVOutput()
{
  *this = xmlNode;
}

CSVOutput& CSVOutput::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode quoteFieldsNode = resultNode.FirstChild("QuoteFields");
    if(!quoteFieldsNode.IsNull())
    {
      m_quoteFields = QuoteFieldsMapper::GetQuoteFieldsForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(quoteFieldsNode.GetText()).c_str()).c_str());
      m_quoteFieldsHasBeenSet = true;
    }
    XmlNode quoteEscapeCharacterNode = resultNode.FirstChild("QuoteEscapeCharacter");
    if(!quoteEscapeCharacterNode.IsNull())
    {
      m_quoteEscapeCharacter = Aws::Utils::Xml::DecodeEscapedXmlText(quoteEscapeCharacterNode.GetText());
      m_quoteEscapeCharacterHasBeenSet = true;
    }
    XmlNode recordDelimiterNode = resultNode.FirstChild("RecordDelimiter");
    if(!recordDelimiterNode.IsNull())
    {
      m_recordDelimiter = Aws::Utils::Xml::DecodeEscapedXmlText(recordDelimiterNode.GetText());
      m_recordDelimiterHasBeenSet = true;
    }
    XmlNode fieldDelimiterNode = resultNode.FirstChild("FieldDelimiter");
    if(!fieldDelimiterNode.IsNull())
    {
      m_fieldDelimiter = Aws::Utils::Xml::DecodeEscapedXmlText(fieldDelimiterNode.GetText());
      m_fieldDelimiterHasBeenSet = true;
    }
    XmlNode quoteCharacterNode = resultNode.FirstChild("QuoteCharacter");
    if(!quoteCharacterNode.IsNull())
    {
      m_quoteCharacter = Aws::Utils::Xml::DecodeEscapedXmlText(quoteCharacterNode.GetText());
      m_quoteCharacterHasBeenSet = true;
    }
  }

  return *this;
}

void CSVOutput::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_quoteFieldsHasBeenSet)
  {
   XmlNode quoteFieldsNode = parentNode.CreateChildElement("QuoteFields");
   quoteFieldsNode.SetText(QuoteFieldsMapper::GetNameForQuoteFields(m_quoteFields));
  }

  if(m_quoteEscapeCharacterHasBeenSet)
  {
   XmlNode quoteEscapeCharacterNode = parentNode.CreateChildElement("QuoteEscapeCharacter");
   quoteEscapeCharacterNode.SetText(m_quoteEscapeCharacter);
  }

  if(m_recordDelimiterHasBeenSet)
  {
   XmlNode recordDelimiterNode = parentNode.CreateChildElement("RecordDelimiter");
   recordDelimiterNode.SetText(m_recordDelimiter);
  }

  if(m_fieldDelimiterHasBeenSet)
  {
   XmlNode fieldDelimiterNode = parentNode.CreateChildElement("FieldDelimiter");
   fieldDelimiterNode.SetText(m_fieldDelimiter);
  }

  if(m_quoteCharacterHasBeenSet)
  {
   XmlNode quoteCharacterNode = parentNode.CreateChildElement("QuoteCharacter");
   quoteCharacterNode.SetText(m_quoteCharacter);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
