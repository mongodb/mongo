/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/AbortIncompleteMultipartUpload.h>
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

AbortIncompleteMultipartUpload::AbortIncompleteMultipartUpload() : 
    m_daysAfterInitiation(0),
    m_daysAfterInitiationHasBeenSet(false)
{
}

AbortIncompleteMultipartUpload::AbortIncompleteMultipartUpload(const XmlNode& xmlNode)
  : AbortIncompleteMultipartUpload()
{
  *this = xmlNode;
}

AbortIncompleteMultipartUpload& AbortIncompleteMultipartUpload::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode daysAfterInitiationNode = resultNode.FirstChild("DaysAfterInitiation");
    if(!daysAfterInitiationNode.IsNull())
    {
      m_daysAfterInitiation = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(daysAfterInitiationNode.GetText()).c_str()).c_str());
      m_daysAfterInitiationHasBeenSet = true;
    }
  }

  return *this;
}

void AbortIncompleteMultipartUpload::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_daysAfterInitiationHasBeenSet)
  {
   XmlNode daysAfterInitiationNode = parentNode.CreateChildElement("DaysAfterInitiation");
   ss << m_daysAfterInitiation;
   daysAfterInitiationNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
