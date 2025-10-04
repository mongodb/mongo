/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/Position.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace IAM
{
namespace Model
{

Position::Position() : 
    m_line(0),
    m_lineHasBeenSet(false),
    m_column(0),
    m_columnHasBeenSet(false)
{
}

Position::Position(const XmlNode& xmlNode)
  : Position()
{
  *this = xmlNode;
}

Position& Position::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode lineNode = resultNode.FirstChild("Line");
    if(!lineNode.IsNull())
    {
      m_line = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(lineNode.GetText()).c_str()).c_str());
      m_lineHasBeenSet = true;
    }
    XmlNode columnNode = resultNode.FirstChild("Column");
    if(!columnNode.IsNull())
    {
      m_column = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(columnNode.GetText()).c_str()).c_str());
      m_columnHasBeenSet = true;
    }
  }

  return *this;
}

void Position::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_lineHasBeenSet)
  {
      oStream << location << index << locationValue << ".Line=" << m_line << "&";
  }

  if(m_columnHasBeenSet)
  {
      oStream << location << index << locationValue << ".Column=" << m_column << "&";
  }

}

void Position::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_lineHasBeenSet)
  {
      oStream << location << ".Line=" << m_line << "&";
  }
  if(m_columnHasBeenSet)
  {
      oStream << location << ".Column=" << m_column << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
