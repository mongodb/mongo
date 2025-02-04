/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/InventoryDestination.h>
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

InventoryDestination::InventoryDestination() : 
    m_s3BucketDestinationHasBeenSet(false)
{
}

InventoryDestination::InventoryDestination(const XmlNode& xmlNode)
  : InventoryDestination()
{
  *this = xmlNode;
}

InventoryDestination& InventoryDestination::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode s3BucketDestinationNode = resultNode.FirstChild("S3BucketDestination");
    if(!s3BucketDestinationNode.IsNull())
    {
      m_s3BucketDestination = s3BucketDestinationNode;
      m_s3BucketDestinationHasBeenSet = true;
    }
  }

  return *this;
}

void InventoryDestination::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_s3BucketDestinationHasBeenSet)
  {
   XmlNode s3BucketDestinationNode = parentNode.CreateChildElement("S3BucketDestination");
   m_s3BucketDestination.AddToNode(s3BucketDestinationNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
