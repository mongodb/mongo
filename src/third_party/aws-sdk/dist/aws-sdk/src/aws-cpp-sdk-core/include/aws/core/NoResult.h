/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

namespace Aws
{
namespace Utils
{
namespace Xml
{
    class XmlDocument;
}
namespace Json
{
    class JsonValue;
}
} // namespace Utils

class AWS_CORE_API NoResult
{
public:
    NoResult() {};
    NoResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>&) {};
    NoResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>&) {};
};

} // namespace Aws
