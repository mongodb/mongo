/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ErrorDetails.h>
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

ErrorDetails::ErrorDetails() : 
    m_errorCodeHasBeenSet(false),
    m_errorMessageHasBeenSet(false)
{
}

ErrorDetails::ErrorDetails(const XmlNode& xmlNode)
  : ErrorDetails()
{
  *this = xmlNode;
}

ErrorDetails& ErrorDetails::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode errorCodeNode = resultNode.FirstChild("ErrorCode");
    if(!errorCodeNode.IsNull())
    {
      m_errorCode = Aws::Utils::Xml::DecodeEscapedXmlText(errorCodeNode.GetText());
      m_errorCodeHasBeenSet = true;
    }
    XmlNode errorMessageNode = resultNode.FirstChild("ErrorMessage");
    if(!errorMessageNode.IsNull())
    {
      m_errorMessage = Aws::Utils::Xml::DecodeEscapedXmlText(errorMessageNode.GetText());
      m_errorMessageHasBeenSet = true;
    }
  }

  return *this;
}

void ErrorDetails::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_errorCodeHasBeenSet)
  {
   XmlNode errorCodeNode = parentNode.CreateChildElement("ErrorCode");
   errorCodeNode.SetText(m_errorCode);
  }

  if(m_errorMessageHasBeenSet)
  {
   XmlNode errorMessageNode = parentNode.CreateChildElement("ErrorMessage");
   errorMessageNode.SetText(m_errorMessage);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
