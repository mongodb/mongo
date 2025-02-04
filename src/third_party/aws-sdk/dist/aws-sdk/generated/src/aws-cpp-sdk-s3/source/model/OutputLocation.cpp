/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/OutputLocation.h>
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

OutputLocation::OutputLocation() : 
    m_s3HasBeenSet(false)
{
}

OutputLocation::OutputLocation(const XmlNode& xmlNode)
  : OutputLocation()
{
  *this = xmlNode;
}

OutputLocation& OutputLocation::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode s3Node = resultNode.FirstChild("S3");
    if(!s3Node.IsNull())
    {
      m_s3 = s3Node;
      m_s3HasBeenSet = true;
    }
  }

  return *this;
}

void OutputLocation::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_s3HasBeenSet)
  {
   XmlNode s3Node = parentNode.CreateChildElement("S3");
   m_s3.AddToNode(s3Node);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
