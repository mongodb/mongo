/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/CreateBucketConfiguration.h>
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

CreateBucketConfiguration::CreateBucketConfiguration() : 
    m_locationConstraint(BucketLocationConstraint::NOT_SET),
    m_locationConstraintHasBeenSet(false),
    m_locationHasBeenSet(false),
    m_bucketHasBeenSet(false)
{
}

CreateBucketConfiguration::CreateBucketConfiguration(const XmlNode& xmlNode)
  : CreateBucketConfiguration()
{
  *this = xmlNode;
}

CreateBucketConfiguration& CreateBucketConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode locationConstraintNode = resultNode.FirstChild("LocationConstraint");
    if(!locationConstraintNode.IsNull())
    {
      m_locationConstraint = BucketLocationConstraintMapper::GetBucketLocationConstraintForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(locationConstraintNode.GetText()).c_str()).c_str());
      m_locationConstraintHasBeenSet = true;
    }
    XmlNode locationNode = resultNode.FirstChild("Location");
    if(!locationNode.IsNull())
    {
      m_location = locationNode;
      m_locationHasBeenSet = true;
    }
    XmlNode bucketNode = resultNode.FirstChild("Bucket");
    if(!bucketNode.IsNull())
    {
      m_bucket = bucketNode;
      m_bucketHasBeenSet = true;
    }
  }

  return *this;
}

void CreateBucketConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_locationConstraintHasBeenSet)
  {
   XmlNode locationConstraintNode = parentNode.CreateChildElement("LocationConstraint");
   locationConstraintNode.SetText(BucketLocationConstraintMapper::GetNameForBucketLocationConstraint(m_locationConstraint));
  }

  if(m_locationHasBeenSet)
  {
   XmlNode locationNode = parentNode.CreateChildElement("Location");
   m_location.AddToNode(locationNode);
  }

  if(m_bucketHasBeenSet)
  {
   XmlNode bucketNode = parentNode.CreateChildElement("Bucket");
   m_bucket.AddToNode(bucketNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
