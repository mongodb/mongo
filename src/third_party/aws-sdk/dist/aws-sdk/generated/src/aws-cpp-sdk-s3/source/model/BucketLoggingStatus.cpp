/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/BucketLoggingStatus.h>
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

BucketLoggingStatus::BucketLoggingStatus() : 
    m_loggingEnabledHasBeenSet(false)
{
}

BucketLoggingStatus::BucketLoggingStatus(const XmlNode& xmlNode)
  : BucketLoggingStatus()
{
  *this = xmlNode;
}

BucketLoggingStatus& BucketLoggingStatus::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode loggingEnabledNode = resultNode.FirstChild("LoggingEnabled");
    if(!loggingEnabledNode.IsNull())
    {
      m_loggingEnabled = loggingEnabledNode;
      m_loggingEnabledHasBeenSet = true;
    }
  }

  return *this;
}

void BucketLoggingStatus::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_loggingEnabledHasBeenSet)
  {
   XmlNode loggingEnabledNode = parentNode.CreateChildElement("LoggingEnabled");
   m_loggingEnabled.AddToNode(loggingEnabledNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
