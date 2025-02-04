/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/MetadataTableConfigurationResult.h>
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

MetadataTableConfigurationResult::MetadataTableConfigurationResult() : 
    m_s3TablesDestinationResultHasBeenSet(false)
{
}

MetadataTableConfigurationResult::MetadataTableConfigurationResult(const XmlNode& xmlNode)
  : MetadataTableConfigurationResult()
{
  *this = xmlNode;
}

MetadataTableConfigurationResult& MetadataTableConfigurationResult::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode s3TablesDestinationResultNode = resultNode.FirstChild("S3TablesDestinationResult");
    if(!s3TablesDestinationResultNode.IsNull())
    {
      m_s3TablesDestinationResult = s3TablesDestinationResultNode;
      m_s3TablesDestinationResultHasBeenSet = true;
    }
  }

  return *this;
}

void MetadataTableConfigurationResult::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_s3TablesDestinationResultHasBeenSet)
  {
   XmlNode s3TablesDestinationResultNode = parentNode.CreateChildElement("S3TablesDestinationResult");
   m_s3TablesDestinationResult.AddToNode(s3TablesDestinationResultNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
