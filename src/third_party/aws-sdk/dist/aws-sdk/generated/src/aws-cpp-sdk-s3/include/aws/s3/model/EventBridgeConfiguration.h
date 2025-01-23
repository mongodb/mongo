/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{

  /**
   * <p>A container for specifying the configuration for Amazon
   * EventBridge.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/EventBridgeConfiguration">AWS
   * API Reference</a></p>
   */
  class EventBridgeConfiguration
  {
  public:
    AWS_S3_API EventBridgeConfiguration();
    AWS_S3_API EventBridgeConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API EventBridgeConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;

  };

} // namespace Model
} // namespace S3
} // namespace Aws
