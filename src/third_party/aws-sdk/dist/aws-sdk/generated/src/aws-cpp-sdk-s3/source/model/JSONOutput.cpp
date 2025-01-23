/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/JSONOutput.h>
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

JSONOutput::JSONOutput() : 
    m_recordDelimiterHasBeenSet(false)
{
}

JSONOutput::JSONOutput(const XmlNode& xmlNode)
  : JSONOutput()
{
  *this = xmlNode;
}

JSONOutput& JSONOutput::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode recordDelimiterNode = resultNode.FirstChild("RecordDelimiter");
    if(!recordDelimiterNode.IsNull())
    {
      m_recordDelimiter = Aws::Utils::Xml::DecodeEscapedXmlText(recordDelimiterNode.GetText());
      m_recordDelimiterHasBeenSet = true;
    }
  }

  return *this;
}

void JSONOutput::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_recordDelimiterHasBeenSet)
  {
   XmlNode recordDelimiterNode = parentNode.CreateChildElement("RecordDelimiter");
   recordDelimiterNode.SetText(m_recordDelimiter);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
