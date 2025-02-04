/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/http/HttpTypes.h>

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

  class SelectObjectContentInitialResponse
  {
  public:
    AWS_S3_API SelectObjectContentInitialResponse();
    AWS_S3_API SelectObjectContentInitialResponse(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API SelectObjectContentInitialResponse& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;

  };

} // namespace Model
} // namespace S3
} // namespace Aws
