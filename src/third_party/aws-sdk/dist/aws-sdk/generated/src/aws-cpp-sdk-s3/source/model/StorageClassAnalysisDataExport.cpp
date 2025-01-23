/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/StorageClassAnalysisDataExport.h>
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

StorageClassAnalysisDataExport::StorageClassAnalysisDataExport() : 
    m_outputSchemaVersion(StorageClassAnalysisSchemaVersion::NOT_SET),
    m_outputSchemaVersionHasBeenSet(false),
    m_destinationHasBeenSet(false)
{
}

StorageClassAnalysisDataExport::StorageClassAnalysisDataExport(const XmlNode& xmlNode)
  : StorageClassAnalysisDataExport()
{
  *this = xmlNode;
}

StorageClassAnalysisDataExport& StorageClassAnalysisDataExport::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode outputSchemaVersionNode = resultNode.FirstChild("OutputSchemaVersion");
    if(!outputSchemaVersionNode.IsNull())
    {
      m_outputSchemaVersion = StorageClassAnalysisSchemaVersionMapper::GetStorageClassAnalysisSchemaVersionForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(outputSchemaVersionNode.GetText()).c_str()).c_str());
      m_outputSchemaVersionHasBeenSet = true;
    }
    XmlNode destinationNode = resultNode.FirstChild("Destination");
    if(!destinationNode.IsNull())
    {
      m_destination = destinationNode;
      m_destinationHasBeenSet = true;
    }
  }

  return *this;
}

void StorageClassAnalysisDataExport::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_outputSchemaVersionHasBeenSet)
  {
   XmlNode outputSchemaVersionNode = parentNode.CreateChildElement("OutputSchemaVersion");
   outputSchemaVersionNode.SetText(StorageClassAnalysisSchemaVersionMapper::GetNameForStorageClassAnalysisSchemaVersion(m_outputSchemaVersion));
  }

  if(m_destinationHasBeenSet)
  {
   XmlNode destinationNode = parentNode.CreateChildElement("Destination");
   m_destination.AddToNode(destinationNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
