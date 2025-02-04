/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/VersioningConfiguration.h>
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

VersioningConfiguration::VersioningConfiguration() : 
    m_mFADelete(MFADelete::NOT_SET),
    m_mFADeleteHasBeenSet(false),
    m_status(BucketVersioningStatus::NOT_SET),
    m_statusHasBeenSet(false)
{
}

VersioningConfiguration::VersioningConfiguration(const XmlNode& xmlNode)
  : VersioningConfiguration()
{
  *this = xmlNode;
}

VersioningConfiguration& VersioningConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode mFADeleteNode = resultNode.FirstChild("MfaDelete");
    if(!mFADeleteNode.IsNull())
    {
      m_mFADelete = MFADeleteMapper::GetMFADeleteForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(mFADeleteNode.GetText()).c_str()).c_str());
      m_mFADeleteHasBeenSet = true;
    }
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = BucketVersioningStatusMapper::GetBucketVersioningStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
      m_statusHasBeenSet = true;
    }
  }

  return *this;
}

void VersioningConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_mFADeleteHasBeenSet)
  {
   XmlNode mFADeleteNode = parentNode.CreateChildElement("MfaDelete");
   mFADeleteNode.SetText(MFADeleteMapper::GetNameForMFADelete(m_mFADelete));
  }

  if(m_statusHasBeenSet)
  {
   XmlNode statusNode = parentNode.CreateChildElement("Status");
   statusNode.SetText(BucketVersioningStatusMapper::GetNameForBucketVersioningStatus(m_status));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
