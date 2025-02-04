/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/InventoryEncryption.h>
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

InventoryEncryption::InventoryEncryption() : 
    m_sSES3HasBeenSet(false),
    m_sSEKMSHasBeenSet(false)
{
}

InventoryEncryption::InventoryEncryption(const XmlNode& xmlNode)
  : InventoryEncryption()
{
  *this = xmlNode;
}

InventoryEncryption& InventoryEncryption::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode sSES3Node = resultNode.FirstChild("SSE-S3");
    if(!sSES3Node.IsNull())
    {
      m_sSES3 = sSES3Node;
      m_sSES3HasBeenSet = true;
    }
    XmlNode sSEKMSNode = resultNode.FirstChild("SSE-KMS");
    if(!sSEKMSNode.IsNull())
    {
      m_sSEKMS = sSEKMSNode;
      m_sSEKMSHasBeenSet = true;
    }
  }

  return *this;
}

void InventoryEncryption::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_sSES3HasBeenSet)
  {
   XmlNode sSES3Node = parentNode.CreateChildElement("SSE-S3");
   m_sSES3.AddToNode(sSES3Node);
  }

  if(m_sSEKMSHasBeenSet)
  {
   XmlNode sSEKMSNode = parentNode.CreateChildElement("SSE-KMS");
   m_sSEKMS.AddToNode(sSEKMSNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
