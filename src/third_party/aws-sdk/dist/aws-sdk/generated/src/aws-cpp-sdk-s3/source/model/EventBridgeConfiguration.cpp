/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/EventBridgeConfiguration.h>
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

EventBridgeConfiguration::EventBridgeConfiguration()
{
}

EventBridgeConfiguration::EventBridgeConfiguration(const XmlNode& xmlNode)
{
  *this = xmlNode;
}

EventBridgeConfiguration& EventBridgeConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
  }

  return *this;
}

void EventBridgeConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  AWS_UNREFERENCED_PARAM(parentNode);
}

} // namespace Model
} // namespace S3
} // namespace Aws
