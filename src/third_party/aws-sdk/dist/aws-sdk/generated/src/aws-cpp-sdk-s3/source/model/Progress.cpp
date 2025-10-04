/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Progress.h>
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

Progress::Progress() : 
    m_bytesScanned(0),
    m_bytesScannedHasBeenSet(false),
    m_bytesProcessed(0),
    m_bytesProcessedHasBeenSet(false),
    m_bytesReturned(0),
    m_bytesReturnedHasBeenSet(false)
{
}

Progress::Progress(const XmlNode& xmlNode)
  : Progress()
{
  *this = xmlNode;
}

Progress& Progress::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode bytesScannedNode = resultNode.FirstChild("BytesScanned");
    if(!bytesScannedNode.IsNull())
    {
      m_bytesScanned = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(bytesScannedNode.GetText()).c_str()).c_str());
      m_bytesScannedHasBeenSet = true;
    }
    XmlNode bytesProcessedNode = resultNode.FirstChild("BytesProcessed");
    if(!bytesProcessedNode.IsNull())
    {
      m_bytesProcessed = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(bytesProcessedNode.GetText()).c_str()).c_str());
      m_bytesProcessedHasBeenSet = true;
    }
    XmlNode bytesReturnedNode = resultNode.FirstChild("BytesReturned");
    if(!bytesReturnedNode.IsNull())
    {
      m_bytesReturned = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(bytesReturnedNode.GetText()).c_str()).c_str());
      m_bytesReturnedHasBeenSet = true;
    }
  }

  return *this;
}

void Progress::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_bytesScannedHasBeenSet)
  {
   XmlNode bytesScannedNode = parentNode.CreateChildElement("BytesScanned");
   ss << m_bytesScanned;
   bytesScannedNode.SetText(ss.str());
   ss.str("");
  }

  if(m_bytesProcessedHasBeenSet)
  {
   XmlNode bytesProcessedNode = parentNode.CreateChildElement("BytesProcessed");
   ss << m_bytesProcessed;
   bytesProcessedNode.SetText(ss.str());
   ss.str("");
  }

  if(m_bytesReturnedHasBeenSet)
  {
   XmlNode bytesReturnedNode = parentNode.CreateChildElement("BytesReturned");
   ss << m_bytesReturned;
   bytesReturnedNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
