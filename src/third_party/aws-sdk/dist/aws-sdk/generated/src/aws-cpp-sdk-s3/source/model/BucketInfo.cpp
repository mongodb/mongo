/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/BucketInfo.h>
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

BucketInfo::BucketInfo() : 
    m_dataRedundancy(DataRedundancy::NOT_SET),
    m_dataRedundancyHasBeenSet(false),
    m_type(BucketType::NOT_SET),
    m_typeHasBeenSet(false)
{
}

BucketInfo::BucketInfo(const XmlNode& xmlNode)
  : BucketInfo()
{
  *this = xmlNode;
}

BucketInfo& BucketInfo::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode dataRedundancyNode = resultNode.FirstChild("DataRedundancy");
    if(!dataRedundancyNode.IsNull())
    {
      m_dataRedundancy = DataRedundancyMapper::GetDataRedundancyForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(dataRedundancyNode.GetText()).c_str()).c_str());
      m_dataRedundancyHasBeenSet = true;
    }
    XmlNode typeNode = resultNode.FirstChild("Type");
    if(!typeNode.IsNull())
    {
      m_type = BucketTypeMapper::GetBucketTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(typeNode.GetText()).c_str()).c_str());
      m_typeHasBeenSet = true;
    }
  }

  return *this;
}

void BucketInfo::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_dataRedundancyHasBeenSet)
  {
   XmlNode dataRedundancyNode = parentNode.CreateChildElement("DataRedundancy");
   dataRedundancyNode.SetText(DataRedundancyMapper::GetNameForDataRedundancy(m_dataRedundancy));
  }

  if(m_typeHasBeenSet)
  {
   XmlNode typeNode = parentNode.CreateChildElement("Type");
   typeNode.SetText(BucketTypeMapper::GetNameForBucketType(m_type));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
