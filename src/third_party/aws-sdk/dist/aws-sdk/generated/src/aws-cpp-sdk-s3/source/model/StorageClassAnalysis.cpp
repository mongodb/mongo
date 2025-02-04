/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/StorageClassAnalysis.h>
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

StorageClassAnalysis::StorageClassAnalysis() : 
    m_dataExportHasBeenSet(false)
{
}

StorageClassAnalysis::StorageClassAnalysis(const XmlNode& xmlNode)
  : StorageClassAnalysis()
{
  *this = xmlNode;
}

StorageClassAnalysis& StorageClassAnalysis::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode dataExportNode = resultNode.FirstChild("DataExport");
    if(!dataExportNode.IsNull())
    {
      m_dataExport = dataExportNode;
      m_dataExportHasBeenSet = true;
    }
  }

  return *this;
}

void StorageClassAnalysis::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_dataExportHasBeenSet)
  {
   XmlNode dataExportNode = parentNode.CreateChildElement("DataExport");
   m_dataExport.AddToNode(dataExportNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
