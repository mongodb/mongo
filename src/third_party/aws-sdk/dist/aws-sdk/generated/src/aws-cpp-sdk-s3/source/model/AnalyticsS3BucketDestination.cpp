/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/AnalyticsS3BucketDestination.h>
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

AnalyticsS3BucketDestination::AnalyticsS3BucketDestination() : 
    m_format(AnalyticsS3ExportFileFormat::NOT_SET),
    m_formatHasBeenSet(false),
    m_bucketAccountIdHasBeenSet(false),
    m_bucketHasBeenSet(false),
    m_prefixHasBeenSet(false)
{
}

AnalyticsS3BucketDestination::AnalyticsS3BucketDestination(const XmlNode& xmlNode)
  : AnalyticsS3BucketDestination()
{
  *this = xmlNode;
}

AnalyticsS3BucketDestination& AnalyticsS3BucketDestination::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode formatNode = resultNode.FirstChild("Format");
    if(!formatNode.IsNull())
    {
      m_format = AnalyticsS3ExportFileFormatMapper::GetAnalyticsS3ExportFileFormatForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(formatNode.GetText()).c_str()).c_str());
      m_formatHasBeenSet = true;
    }
    XmlNode bucketAccountIdNode = resultNode.FirstChild("BucketAccountId");
    if(!bucketAccountIdNode.IsNull())
    {
      m_bucketAccountId = Aws::Utils::Xml::DecodeEscapedXmlText(bucketAccountIdNode.GetText());
      m_bucketAccountIdHasBeenSet = true;
    }
    XmlNode bucketNode = resultNode.FirstChild("Bucket");
    if(!bucketNode.IsNull())
    {
      m_bucket = Aws::Utils::Xml::DecodeEscapedXmlText(bucketNode.GetText());
      m_bucketHasBeenSet = true;
    }
    XmlNode prefixNode = resultNode.FirstChild("Prefix");
    if(!prefixNode.IsNull())
    {
      m_prefix = Aws::Utils::Xml::DecodeEscapedXmlText(prefixNode.GetText());
      m_prefixHasBeenSet = true;
    }
  }

  return *this;
}

void AnalyticsS3BucketDestination::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_formatHasBeenSet)
  {
   XmlNode formatNode = parentNode.CreateChildElement("Format");
   formatNode.SetText(AnalyticsS3ExportFileFormatMapper::GetNameForAnalyticsS3ExportFileFormat(m_format));
  }

  if(m_bucketAccountIdHasBeenSet)
  {
   XmlNode bucketAccountIdNode = parentNode.CreateChildElement("BucketAccountId");
   bucketAccountIdNode.SetText(m_bucketAccountId);
  }

  if(m_bucketHasBeenSet)
  {
   XmlNode bucketNode = parentNode.CreateChildElement("Bucket");
   bucketNode.SetText(m_bucket);
  }

  if(m_prefixHasBeenSet)
  {
   XmlNode prefixNode = parentNode.CreateChildElement("Prefix");
   prefixNode.SetText(m_prefix);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
